#include "companion/companionservice.h"

#include <QEventLoop>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <cmath>

#include <QDataStream>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include "companion/companionserver.h"
#include "companion/companionsettings.h"
#include "companion/harmonic.h"
#include "companion/pairingmanager.h"
#include "companion/trackserializer.h"
#include "companion/waveformmxwf.h"
#include "control/controlobject.h"
#include "preferences/configobject.h"
#include <QAbstractItemModel>
#include <QBuffer>
#include <QImage>

#include "library/coverart.h"
#include "library/dao/analysisdao.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackdao.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/searchquery.h"
#include "library/trackmodel.h"
#include "library/searchqueryparser.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"
#include "track/trackid.h"
#include "util/assert.h"
#include "util/color/rgbcolor.h"
#include "util/logger.h"
#include "waveform/waveform.h"
#include "waveform/waveformfactory.h"

namespace {
const mixxx::Logger kLogger("Companion");
} // namespace

namespace mixxx {
namespace companion {

CompanionService::CompanionService(UserSettingsPointer pConfig,
        PlayerManager* pPlayerManager,
        TrackCollectionManager* pTrackCollectionManager,
        Library* pLibrary,
        QString appVersion,
        QObject* parent)
        : QObject(parent),
          m_pConfig(std::move(pConfig)),
          m_pPlayerManager(pPlayerManager),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pLibrary(pLibrary),
          // The code identifies this Mixxx session, not the server's run state:
          // it has to be readable in Preferences before the API is ever enabled,
          // and must survive a restart so a code already typed into a phone
          // keeps working. Generating it costs nothing when the API stays off.
          m_sessionCode(PairingManager::makeCode()),
          m_appVersion(std::move(appVersion)),
          m_pThread(nullptr),
          m_pServer(nullptr),
          m_connectedDecks(0),
          m_running(false) {
}

CompanionService::~CompanionService() {
    stop();
}

void CompanionService::start() {
    if (m_running) {
        return;
    }
    const CompanionSettings settings(m_pConfig);
    if (!settings.isEnabled()) {
        kLogger.info() << "Companion API disabled; not starting";
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(m_pPlayerManager) {
        return;
    }

    const int numDecks = m_pPlayerManager->numberOfDecks();

    m_pThread = new QThread();
    m_pThread->setObjectName(QStringLiteral("CompanionAPI"));

    m_pServer = new CompanionServer(settings.bindAddress(),
            settings.port(),
            settings.tickIntervalMs(),
            m_appVersion,
            numDecks,
            this, // query handler (runLibrarySearch runs on this main thread)
            settings.pairedTokens(),
            m_sessionCode);
    m_pServer->moveToThread(m_pThread);

    connect(this,
            &CompanionService::deckLoadedEvent,
            m_pServer,
            &CompanionServer::onDeckLoaded);
    connect(this,
            &CompanionService::deckBeatgridEvent,
            m_pServer,
            &CompanionServer::onDeckBeatgrid);
    connect(this,
            &CompanionService::deckCuesEvent,
            m_pServer,
            &CompanionServer::onDeckCues);
    connect(this,
            &CompanionService::deckUnloadedEvent,
            m_pServer,
            &CompanionServer::onDeckUnloaded);
    connect(this,
            &CompanionService::numberOfDecksChangedEvent,
            m_pServer,
            &CompanionServer::onNumberOfDecksChanged);
    // Actions flow the other way: server (worker) -> service (main thread).
    connect(m_pServer,
            &CompanionServer::loadToDeckRequested,
            this,
            &CompanionService::onLoadToDeckRequested);
    connect(m_pServer,
            &CompanionServer::autoDjQueueRequested,
            this,
            &CompanionService::onAutoDjQueueRequested);
    connect(m_pServer,
            &CompanionServer::persistTokens,
            this,
            &CompanionService::onPersistTokens);
    connect(this,
            &CompanionService::libraryViewEvent,
            m_pServer,
            &CompanionServer::onLibraryView);
    connect(this,
            &CompanionService::libraryCursorEvent,
            m_pServer,
            &CompanionServer::onLibraryCursor);

    connect(this,
            &CompanionService::libraryChangedEvent,
            m_pServer,
            &CompanionServer::onLibraryChanged);

    // Grid and cue edits both arrive in bursts (dragging a grid emits
    // beatsUpdated per mouse move; clearing hotcues emits cuesUpdated per cue),
    // so coalesce each into at most one send per deck.
    if (!m_pDeckDetailTimer) {
        m_pDeckDetailTimer = new QTimer(this);
        m_pDeckDetailTimer->setSingleShot(true);
        m_pDeckDetailTimer->setInterval(200);
        connect(m_pDeckDetailTimer, &QTimer::timeout, this, [this]() {
            const QSet<int> gridDecks = m_pendingBeatgridDecks;
            m_pendingBeatgridDecks.clear();
            for (int deck : gridDecks) {
                emitBeatgrid(deck);
            }
            const QSet<int> cueDecks = m_pendingCueDecks;
            m_pendingCueDecks.clear();
            for (int deck : cueDecks) {
                emitCues(deck);
            }
        });
    }

    // Coarse library invalidation: coalesce TrackDAO change bursts.
    if (m_pTrackCollectionManager) {
        if (!m_pLibraryChangedTimer) {
            m_pLibraryChangedTimer = new QTimer(this);
            m_pLibraryChangedTimer->setSingleShot(true);
            m_pLibraryChangedTimer->setInterval(500);
            connect(m_pLibraryChangedTimer, &QTimer::timeout, this, [this]() {
                QJsonObject event = m_pendingLibraryChanged;
                m_pendingLibraryChanged = QJsonObject();
                event.insert(QStringLiteral("type"),
                        QStringLiteral("library.changed"));
                emit libraryChangedEvent(event);
            });
        }
        TrackDAO& trackDao =
                m_pTrackCollectionManager->internalCollection()->getTrackDAO();
        connect(&trackDao,
                &TrackDAO::tracksAdded,
                this,
                [this](const QSet<TrackId>& ids) {
                    queueLibraryChanged("tracksAdded", ids);
                });
        connect(&trackDao,
                &TrackDAO::tracksChanged,
                this,
                [this](const QSet<TrackId>& ids) {
                    queueLibraryChanged("tracksChanged", ids);
                });
        connect(&trackDao,
                &TrackDAO::tracksRemoved,
                this,
                [this](const QSet<TrackId>& ids) {
                    queueLibraryChanged("tracksRemoved", ids);
                });
    }

    // Library browse HUD: observe the active view and the highlighted track.
    // Both signals are emitted by the library widgets/features and arrive here
    // on the main thread; trackSelected is debounced (~100 ms) by the view.
    if (m_pLibrary) {
        connect(m_pLibrary,
                &Library::showTrackModel,
                this,
                [this](QAbstractItemModel* pModel) { onShowTrackModel(pModel); });
        connect(m_pLibrary,
                &Library::trackSelected,
                this,
                &CompanionService::onTrackSelected);
    }

    m_pThread->start();
    // Create sockets and listeners on the worker thread once its event loop runs.
    QMetaObject::invokeMethod(m_pServer, "initialize", Qt::QueuedConnection);

    connectDecks(0, numDecks);
    m_connectedDecks = numDecks;
    connect(m_pPlayerManager,
            &PlayerManager::numberOfDecksChanged,
            this,
            &CompanionService::onNumberOfDecksChanged);

    m_running = true;
    kLogger.info() << "Companion API started with" << numDecks << "decks";
    emit stateChanged();
}

void CompanionService::restart() {
    stop();
    start();
}

void CompanionService::regenerateSessionCode() {
    m_sessionCode = PairingManager::makeCode();
    if (m_running && m_pServer) {
        // Only a running server has a code to replace; when stopped, start()
        // will hand it the current one.
        QMetaObject::invokeMethod(m_pServer,
                "setSessionCode",
                Qt::QueuedConnection,
                Q_ARG(QString, m_sessionCode));
    }
    emit stateChanged();
}

void CompanionService::stop() {
    if (!m_running) {
        return;
    }
    m_running = false;

    if (m_pPlayerManager) {
        disconnect(m_pPlayerManager, nullptr, this, nullptr);
    }

    if (m_pServer && m_pThread) {
        // Shut down the server on its own thread, then join. We must NOT block
        // the main thread here with a BlockingQueuedConnection: a worker HTTP
        // handler may be mid-flight in a BlockingQueuedConnection call back to
        // this (main) thread (e.g. a library search). Instead, post shutdown()
        // and spin a local event loop so the main thread keeps servicing those
        // in-flight calls until the server reports stopped(). A timeout guards
        // against a wedged worker.
        QEventLoop loop;
        connect(m_pServer,
                &CompanionServer::stopped,
                &loop,
                &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        QMetaObject::invokeMethod(m_pServer, "shutdown", Qt::QueuedConnection);
        loop.exec();
        m_pThread->quit();
        m_pThread->wait();
        // The thread has stopped, so it is safe to delete the (thread-affine)
        // server from here. Mirrors CoreServices' handling of ControllerManager.
        delete m_pServer;
        m_pServer = nullptr;
        delete m_pThread;
        m_pThread = nullptr;
    }
    // m_sessionCode deliberately survives: restart() is stop()+start(), and
    // rotating the code there would silently invalidate an already-paired phone
    // just because the port or the LAN toggle changed.
    kLogger.info() << "Companion API stopped";
    emit stateChanged();
}

void CompanionService::connectDecks(int fromIndex, int toIndex) {
    for (int i = fromIndex; i < toIndex; ++i) {
        BaseTrackPlayer* pPlayer = m_pPlayerManager->getDeckBase(i);
        if (!pPlayer) {
            continue;
        }
        connect(pPlayer,
                &BaseTrackPlayer::newTrackLoaded,
                this,
                [this, i](const TrackPointer& pTrack) { onDeckLoaded(i, pTrack); });
        connect(pPlayer,
                &BaseTrackPlayer::trackUnloaded,
                this,
                [this, i](const TrackPointer&) { onDeckUnloaded(i); });
    }
}

void CompanionService::onDeckLoaded(int deckIndex, const TrackPointer& pTrack) {
    const int deck = deckIndex + 1;
    const quint64 generation = ++m_generations[deck];
    const CompanionSettings settings(m_pConfig);
    const QJsonObject track = serializeTrack(pTrack, settings.exposeFilePaths());
    emit deckLoadedEvent(deck, generation, track);
    // Emitted after the load event (same queued connection, so ordering holds):
    // the server drops a grid/cue event for a deck it does not yet consider
    // loaded.
    watchTrack(deck, pTrack);
    emitBeatgrid(deck);
    emitCues(deck);
}

void CompanionService::onDeckUnloaded(int deckIndex) {
    const int deck = deckIndex + 1;
    const quint64 generation = ++m_generations[deck];
    watchTrack(deck, TrackPointer());
    emit deckUnloadedEvent(deck, generation);
}

void CompanionService::watchTrack(int deck, const TrackPointer& pTrack) {
    const QList<QMetaObject::Connection> previous = m_trackConnections.take(deck);
    for (const QMetaObject::Connection& connection : previous) {
        disconnect(connection);
    }
    m_pendingBeatgridDecks.remove(deck);
    m_pendingCueDecks.remove(deck);
    if (!pTrack) {
        m_deckTracks.remove(deck);
        return;
    }
    m_deckTracks.insert(deck, pTrack);
    // Both change after the load: the analyzer finishes, the user taps BPM or
    // drags the grid, sets or clears a hotcue. The phone needs those, not just
    // whatever happened to be true at load time.
    QList<QMetaObject::Connection> connections;
    connections.append(
            connect(pTrack.get(), &Track::beatsUpdated, this, [this, deck]() {
                queueBeatgrid(deck);
            }));
    connections.append(
            connect(pTrack.get(), &Track::cuesUpdated, this, [this, deck]() {
                queueCues(deck);
            }));
    m_trackConnections.insert(deck, connections);
}

void CompanionService::queueBeatgrid(int deck) {
    m_pendingBeatgridDecks.insert(deck);
    startDeckDetailTimer();
}

void CompanionService::queueCues(int deck) {
    m_pendingCueDecks.insert(deck);
    startDeckDetailTimer();
}

void CompanionService::startDeckDetailTimer() {
    if (m_pDeckDetailTimer && !m_pDeckDetailTimer->isActive()) {
        m_pDeckDetailTimer->start();
    }
}

void CompanionService::emitBeatgrid(int deck) {
    const TrackPointer pTrack = m_deckTracks.value(deck);
    if (!pTrack) {
        return;
    }
    emit deckBeatgridEvent(
            deck, m_generations.value(deck), serializeBeatgrid(pTrack));
}

void CompanionService::emitCues(int deck) {
    const TrackPointer pTrack = m_deckTracks.value(deck);
    if (!pTrack) {
        return;
    }
    emit deckCuesEvent(deck, m_generations.value(deck), serializeCues(pTrack));
}

void CompanionService::onNumberOfDecksChanged(int numDecks) {
    if (numDecks > m_connectedDecks) {
        connectDecks(m_connectedDecks, numDecks);
        m_connectedDecks = numDecks;
    }
    emit numberOfDecksChangedEvent(numDecks);
}

QByteArray CompanionService::runLibrarySearch(const QString& q,
        int bpmMin,
        int bpmMax,
        const QString& key,
        int limit,
        int offset) {
    // Runs on the main thread (invoked via BlockingQueuedConnection). Mirrors
    // BaseTrackCache: build the WHERE with Mixxx's own SearchQueryParser, then
    // run a plain SELECT on the collection's main-thread DB connection.
    QJsonObject result;
    result.insert(QStringLiteral("query"), q);
    result.insert(QStringLiteral("offset"), offset);
    result.insert(QStringLiteral("limit"), limit);
    QJsonArray tracks;

    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        result.insert(QStringLiteral("total"), 0);
        result.insert(QStringLiteral("tracks"), tracks);
        return QJsonDocument(result).toJson(QJsonDocument::Compact);
    }
    TrackCollection* pCollection = m_pTrackCollectionManager->internalCollection();

    // Bare, unqualified columns matched by a plain search term. "location" and
    // "crate" are intentionally omitted (the former is ambiguous across the
    // join, the latter needs crate storage traversal).
    const QStringList searchColumns = {
            QStringLiteral("artist"),
            QStringLiteral("album"),
            QStringLiteral("album_artist"),
            QStringLiteral("grouping"),
            QStringLiteral("comment"),
            QStringLiteral("title"),
            QStringLiteral("genre")};
    SearchQueryParser parser(pCollection, searchColumns);
    const std::unique_ptr<QueryNode> pNode = parser.parseQuery(q, QString());
    const QString parsedWhere = pNode ? pNode->toSql() : QString();

    // Convenience filters ANDed on top of the parsed query. These use bound
    // parameters; the parsed WHERE is already escaped by the parser.
    QStringList extraConditions;
    if (bpmMin > 0) {
        extraConditions << QStringLiteral("bpm >= :bpmMin");
    }
    if (bpmMax > 0) {
        extraConditions << QStringLiteral("bpm <= :bpmMax");
    }
    if (!key.isEmpty()) {
        extraConditions << QStringLiteral("key = :key");
    }

    QString whereClause = QStringLiteral(
            "library.mixxx_deleted = 0 AND track_locations.fs_deleted = 0");
    if (!parsedWhere.isEmpty()) {
        whereClause += QStringLiteral(" AND (") + parsedWhere + QChar(')');
    }
    for (const QString& cond : extraConditions) {
        whereClause += QStringLiteral(" AND ") + cond;
    }

    const QString fromWhere = QStringLiteral(
            "FROM library "
            "INNER JOIN track_locations "
            "ON library.location = track_locations.id "
            "WHERE ") + whereClause;

    QSqlDatabase db = pCollection->database();
    const QSet<int> playedIds = currentSessionTrackIds(db);

    // Total (capped at 1000; a value of 1000 means ">= 1000").
    int total = 0;
    {
        QSqlQuery countQuery(db);
        countQuery.prepare(QStringLiteral("SELECT COUNT(*) ") + fromWhere);
        if (bpmMin > 0) {
            countQuery.bindValue(QStringLiteral(":bpmMin"), bpmMin);
        }
        if (bpmMax > 0) {
            countQuery.bindValue(QStringLiteral(":bpmMax"), bpmMax);
        }
        if (!key.isEmpty()) {
            countQuery.bindValue(QStringLiteral(":key"), key);
        }
        if (countQuery.exec() && countQuery.next()) {
            total = qMin(countQuery.value(0).toInt(), 1000);
        } else {
            kLogger.warning() << "search count failed:"
                              << countQuery.lastError().text();
        }
    }

    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT library.id, artist, title, album, bpm, key, "
            "duration, rating, timesplayed, last_played_at ") +
            fromWhere +
            QStringLiteral(
                    " ORDER BY artist ASC, title ASC LIMIT :limit OFFSET :offset"));
    if (bpmMin > 0) {
        query.bindValue(QStringLiteral(":bpmMin"), bpmMin);
    }
    if (bpmMax > 0) {
        query.bindValue(QStringLiteral(":bpmMax"), bpmMax);
    }
    if (!key.isEmpty()) {
        query.bindValue(QStringLiteral(":key"), key);
    }
    query.bindValue(QStringLiteral(":limit"), limit);
    query.bindValue(QStringLiteral(":offset"), offset);

    if (!query.exec()) {
        kLogger.warning() << "search query failed:" << query.lastError().text();
    } else {
        while (query.next()) {
            QJsonObject row;
            const int trackId = query.value(0).toInt();
            row.insert(QStringLiteral("id"), trackId);
            row.insert(QStringLiteral("title"), query.value(2).toString());
            row.insert(QStringLiteral("artist"), query.value(1).toString());
            if (playedIds.contains(trackId)) {
                row.insert(QStringLiteral("playedTonight"), true);
            }
            const QString album = query.value(3).toString();
            if (!album.isEmpty()) {
                row.insert(QStringLiteral("album"), album);
            }
            const double bpm = query.value(4).toDouble();
            if (bpm > 0.0) {
                row.insert(QStringLiteral("bpm"), bpm);
            }
            const QString keyText = query.value(5).toString();
            if (!keyText.isEmpty()) {
                row.insert(QStringLiteral("key"), keyText);
            }
            row.insert(QStringLiteral("durationSeconds"), query.value(6).toDouble());
            const int rating = query.value(7).toInt();
            if (rating > 0) {
                row.insert(QStringLiteral("rating"), rating);
            }
            const QVariant lastPlayed = query.value(9);
            if (!lastPlayed.isNull()) {
                const QDateTime dt = lastPlayed.toDateTime();
                if (dt.isValid()) {
                    row.insert(QStringLiteral("lastPlayedAtIso"),
                            dt.toString(Qt::ISODate));
                }
            }
            tracks.append(row);
        }
    }

    result.insert(QStringLiteral("total"), total);
    result.insert(QStringLiteral("tracks"), tracks);
    return QJsonDocument(result).toJson(QJsonDocument::Compact);
}

QByteArray CompanionService::getTrackJson(int trackId) {
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    const QVariant idVariant(trackId);
    const TrackId id(idVariant);
    if (!id.isValid()) {
        return QByteArray();
    }
    const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(id);
    if (!pTrack) {
        return QByteArray();
    }
    const CompanionSettings settings(m_pConfig);
    const QJsonObject dto = serializeTrack(pTrack, settings.exposeFilePaths());
    return QJsonDocument(dto).toJson(QJsonDocument::Compact);
}

QByteArray CompanionService::getTrackBeatgrid(int trackId) {
    // Main thread (Track access). Beat positions in seconds, so the phone can
    // draw real beat/bar ticks instead of extrapolating from BPM.
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    const QVariant idVariant(trackId);
    const TrackId id(idVariant);
    if (!id.isValid()) {
        return QByteArray();
    }
    const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(id);
    if (!pTrack) {
        return QByteArray();
    }

    QJsonObject root = serializeBeatgrid(pTrack);
    root.insert(QStringLiteral("trackId"), trackId);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray CompanionService::getTrackCues(int trackId) {
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    const QVariant idVariant(trackId);
    const TrackId id(idVariant);
    if (!id.isValid()) {
        return QByteArray();
    }
    const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(id);
    if (!pTrack) {
        return QByteArray();
    }
    QJsonObject root = serializeCues(pTrack);
    root.insert(QStringLiteral("trackId"), trackId);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray CompanionService::exportWaveformSummary(int trackId) {
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    const QVariant idVariant(trackId);
    const TrackId id(idVariant);
    if (!id.isValid()) {
        return QByteArray();
    }
    TrackCollection* pCollection = m_pTrackCollectionManager->internalCollection();

    // Read the stored summary waveform straight from AnalysisDao (works for any
    // analyzed library track, loaded or not). Reuses the collection's
    // main-thread DB connection.
    AnalysisDao analysisDao(m_pConfig);
    analysisDao.initialize(pCollection->database());
    const QList<AnalysisDao::AnalysisInfo> analyses =
            analysisDao.getAnalysesForTrackByType(
                    id, AnalysisDao::AnalysisType::TYPE_WAVESUMMARY);
    if (analyses.isEmpty()) {
        return QByteArray(); // -> 202 analysis_pending
    }
    const ConstWaveformPointer pWaveform(
            WaveformFactory::loadWaveformFromAnalysis(analyses.first()));
    if (!pWaveform || pWaveform->getDataSize() <= 0) {
        return QByteArray();
    }

    double durationSeconds = 0.0;
    const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(id);
    if (pTrack) {
        durationSeconds = pTrack->getDuration();
    }
    return encodeWaveformSummaryMxwf(
            static_cast<quint32>(trackId), *pWaveform, durationSeconds);
}

void CompanionService::onAutoDjQueueRequested(int trackId) {
    // Runs on the main thread (DAO access).
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return;
    }
    const QVariant idVariant(trackId);
    const TrackId id(idVariant);
    if (!id.isValid()) {
        return;
    }
    const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(id);
    if (!pTrack) {
        kLogger.warning() << "autodj queue: no track with id" << trackId;
        return;
    }
    PlaylistDAO& playlistDao =
            m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
    const int autoDjId = playlistDao.getPlaylistIdFromName(AUTODJ_TABLE);
    if (autoDjId < 0) {
        kLogger.warning() << "autodj queue: no Auto DJ playlist";
        return;
    }
    playlistDao.appendTrackToPlaylist(id, autoDjId);
    kLogger.debug() << "autodj queue: appended track" << trackId;
}

namespace {
// Shared SELECT column list + row serialization for playlist/crate/history
// track listings (matches TrackSearchRow in the spec).
const char* kTrackRowColumns =
        "library.id, artist, title, album, bpm, key, duration, rating";

QJsonObject trackRowFromQuery(const QSqlQuery& query) {
    QJsonObject row;
    row.insert(QStringLiteral("id"), query.value(0).toInt());
    row.insert(QStringLiteral("artist"), query.value(1).toString());
    row.insert(QStringLiteral("title"), query.value(2).toString());
    const QString album = query.value(3).toString();
    if (!album.isEmpty()) {
        row.insert(QStringLiteral("album"), album);
    }
    const double bpm = query.value(4).toDouble();
    if (bpm > 0.0) {
        row.insert(QStringLiteral("bpm"), bpm);
    }
    const QString key = query.value(5).toString();
    if (!key.isEmpty()) {
        row.insert(QStringLiteral("key"), key);
    }
    row.insert(QStringLiteral("durationSeconds"), query.value(6).toDouble());
    const int rating = query.value(7).toInt();
    if (rating > 0) {
        row.insert(QStringLiteral("rating"), rating);
    }
    return row;
}

QByteArray compactJson(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
} // namespace

QByteArray CompanionService::getPlaylists() {
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT id, name, "
            "  (SELECT COUNT(*) FROM PlaylistTracks "
            "   WHERE PlaylistTracks.playlist_id = Playlists.id) "
            "FROM Playlists WHERE hidden = 0 ORDER BY position"));
    QJsonArray playlists;
    if (query.exec()) {
        while (query.next()) {
            QJsonObject playlist;
            playlist.insert(QStringLiteral("id"), query.value(0).toInt());
            playlist.insert(QStringLiteral("name"), query.value(1).toString());
            playlist.insert(
                    QStringLiteral("trackCount"), query.value(2).toInt());
            playlists.append(playlist);
        }
    } else {
        kLogger.warning() << "playlists query failed:"
                          << query.lastError().text();
    }
    QJsonObject root;
    root.insert(QStringLiteral("playlists"), playlists);
    return compactJson(root);
}

QByteArray CompanionService::getPlaylistTracks(int playlistId) {
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    QSqlDatabase db =
            m_pTrackCollectionManager->internalCollection()->database();
    // Distinguish "no such playlist" (-> 404) from "empty playlist".
    QSqlQuery existsQuery(db);
    existsQuery.prepare(QStringLiteral(
            "SELECT name FROM Playlists WHERE id = :id AND hidden = 0"));
    existsQuery.bindValue(QStringLiteral(":id"), playlistId);
    if (!existsQuery.exec() || !existsQuery.next()) {
        return QByteArray();
    }
    const QString name = existsQuery.value(0).toString();

    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
                          "SELECT %1, PlaylistTracks.position "
                          "FROM PlaylistTracks "
                          "INNER JOIN library ON PlaylistTracks.track_id = library.id "
                          "INNER JOIN track_locations "
                          "ON library.location = track_locations.id "
                          "WHERE PlaylistTracks.playlist_id = :id "
                          "AND library.mixxx_deleted = 0 "
                          "AND track_locations.fs_deleted = 0 "
                          "ORDER BY PlaylistTracks.position")
                          .arg(QLatin1String(kTrackRowColumns)));
    query.bindValue(QStringLiteral(":id"), playlistId);
    QJsonArray tracks;
    if (query.exec()) {
        while (query.next()) {
            QJsonObject row = trackRowFromQuery(query);
            row.insert(QStringLiteral("position"), query.value(8).toInt());
            tracks.append(row);
        }
    }
    QJsonObject root;
    root.insert(QStringLiteral("id"), playlistId);
    root.insert(QStringLiteral("name"), name);
    root.insert(QStringLiteral("tracks"), tracks);
    return compactJson(root);
}

QByteArray CompanionService::getCrates() {
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT id, name, "
            "  (SELECT COUNT(*) FROM crate_tracks "
            "   WHERE crate_tracks.crate_id = crates.id) "
            "FROM crates ORDER BY name"));
    QJsonArray crates;
    if (query.exec()) {
        while (query.next()) {
            QJsonObject crate;
            crate.insert(QStringLiteral("id"), query.value(0).toInt());
            crate.insert(QStringLiteral("name"), query.value(1).toString());
            crate.insert(QStringLiteral("trackCount"), query.value(2).toInt());
            crates.append(crate);
        }
    } else {
        kLogger.warning() << "crates query failed:" << query.lastError().text();
    }
    QJsonObject root;
    root.insert(QStringLiteral("crates"), crates);
    return compactJson(root);
}

QByteArray CompanionService::getCrateTracks(int crateId) {
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    QSqlDatabase db =
            m_pTrackCollectionManager->internalCollection()->database();
    QSqlQuery existsQuery(db);
    existsQuery.prepare(
            QStringLiteral("SELECT name FROM crates WHERE id = :id"));
    existsQuery.bindValue(QStringLiteral(":id"), crateId);
    if (!existsQuery.exec() || !existsQuery.next()) {
        return QByteArray();
    }
    const QString name = existsQuery.value(0).toString();

    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
                          "SELECT %1 FROM crate_tracks "
                          "INNER JOIN library ON crate_tracks.track_id = library.id "
                          "INNER JOIN track_locations "
                          "ON library.location = track_locations.id "
                          "WHERE crate_tracks.crate_id = :id "
                          "AND library.mixxx_deleted = 0 "
                          "AND track_locations.fs_deleted = 0 "
                          "ORDER BY artist, title")
                          .arg(QLatin1String(kTrackRowColumns)));
    query.bindValue(QStringLiteral(":id"), crateId);
    QJsonArray tracks;
    if (query.exec()) {
        while (query.next()) {
            tracks.append(trackRowFromQuery(query));
        }
    }
    QJsonObject root;
    root.insert(QStringLiteral("id"), crateId);
    root.insert(QStringLiteral("name"), name);
    root.insert(QStringLiteral("tracks"), tracks);
    return compactJson(root);
}

QByteArray CompanionService::getHistoryTracks() {
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    QSqlDatabase db =
            m_pTrackCollectionManager->internalCollection()->database();
    // The newest set-log playlist is the current session's history.
    QSqlQuery latestQuery(db);
    latestQuery.prepare(QStringLiteral(
            "SELECT id, name FROM Playlists WHERE hidden = 2 "
            "ORDER BY id DESC LIMIT 1"));
    if (!latestQuery.exec() || !latestQuery.next()) {
        // No history yet this session: empty but valid.
        QJsonObject root;
        root.insert(QStringLiteral("tracks"), QJsonArray());
        return compactJson(root);
    }
    const int playlistId = latestQuery.value(0).toInt();
    const QString name = latestQuery.value(1).toString();

    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
                          "SELECT %1, PlaylistTracks.position, "
                          "PlaylistTracks.pl_datetime_added "
                          "FROM PlaylistTracks "
                          "INNER JOIN library ON PlaylistTracks.track_id = library.id "
                          "INNER JOIN track_locations "
                          "ON library.location = track_locations.id "
                          "WHERE PlaylistTracks.playlist_id = :id "
                          "ORDER BY PlaylistTracks.position")
                          .arg(QLatin1String(kTrackRowColumns)));
    query.bindValue(QStringLiteral(":id"), playlistId);
    QJsonArray tracks;
    if (query.exec()) {
        while (query.next()) {
            QJsonObject row = trackRowFromQuery(query);
            row.insert(QStringLiteral("position"), query.value(8).toInt());
            const QDateTime playedAt = query.value(9).toDateTime();
            if (playedAt.isValid()) {
                row.insert(QStringLiteral("playedAtIso"),
                        playedAt.toString(Qt::ISODate));
            }
            tracks.append(row);
        }
    }
    QJsonObject root;
    root.insert(QStringLiteral("id"), playlistId);
    root.insert(QStringLiteral("name"), name);
    root.insert(QStringLiteral("tracks"), tracks);
    return compactJson(root);
}

QSet<int> CompanionService::currentSessionTrackIds(QSqlDatabase& db) const {
    // The newest set-log playlist (hidden == 2) is the current session.
    QSet<int> ids;
    QSqlQuery latest(db);
    latest.prepare(QStringLiteral(
            "SELECT id FROM Playlists WHERE hidden = 2 ORDER BY id DESC LIMIT 1"));
    if (!latest.exec() || !latest.next()) {
        return ids;
    }
    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT track_id FROM PlaylistTracks WHERE playlist_id = :id"));
    query.bindValue(QStringLiteral(":id"), latest.value(0).toInt());
    if (query.exec()) {
        while (query.next()) {
            ids.insert(query.value(0).toInt());
        }
    }
    return ids;
}

QByteArray CompanionService::getDeckSuggestions(int deck, int limit, int bpmWindow) {
    // Main thread (Track + DB access). Turns the app from a mirror into a tool:
    // given what is on `deck`, score compatible next tracks by harmonic key,
    // tempo, energy direction and session freshness. Scoring itself lives in the
    // pure, unit-tested harmonic core; this method only gathers candidates and
    // ranks them.
    using track::io::key::ChromaticKey;

    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    const TrackPointer pFrom = m_deckTracks.value(deck);
    if (!pFrom) {
        return QByteArray(); // -> 404: nothing loaded to suggest against
    }

    const ChromaticKey fromKey = pFrom->getKey();
    // Prefer the deck's live (rate-adjusted) BPM so suggestions honor the fader.
    double fromBpm = pFrom->getBpm();
    const double liveBpm = ControlObject::get(ConfigKey(
            PlayerManager::groupForDeck(deck - 1), QStringLiteral("bpm")));
    if (liveBpm > 0.0) {
        fromBpm = liveBpm;
    }
    const int fromId = pFrom->getId().toVariant().toInt();

    if (bpmWindow <= 0) {
        bpmWindow = 8; // default +-8 BPM candidate window
    }

    TrackCollection* pCollection = m_pTrackCollectionManager->internalCollection();
    QSqlDatabase db = pCollection->database();
    const QSet<int> playedIds = currentSessionTrackIds(db);

    // Prefilter in SQL: within the BPM window, and either harmonically
    // compatible or key-unknown (so unanalyzed tracks still surface). Known
    // clashing keys are excluded here to keep the C++ scoring set small.
    QStringList conditions;
    conditions << QStringLiteral("library.mixxx_deleted = 0")
               << QStringLiteral("track_locations.fs_deleted = 0")
               << QStringLiteral("library.id <> :fromId")
               << QStringLiteral("bpm > 0");
    if (fromBpm > 0.0) {
        conditions << QStringLiteral("bpm BETWEEN :bpmLo AND :bpmHi");
    }
    if (fromKey != track::io::key::INVALID) {
        QStringList compatibleKeys;
        for (int k = track::io::key::C_MAJOR; k <= track::io::key::B_MINOR; ++k) {
            const harmonic::KeyRelation rel =
                    harmonic::relation(fromKey, static_cast<ChromaticKey>(k));
            if (rel == harmonic::KeyRelation::Same ||
                    rel == harmonic::KeyRelation::RelativeMode ||
                    rel == harmonic::KeyRelation::Adjacent) {
                compatibleKeys << QString::number(k);
            }
        }
        if (!compatibleKeys.isEmpty()) {
            conditions << (QStringLiteral("(key_id IN (") +
                    compatibleKeys.join(QLatin1Char(',')) +
                    QStringLiteral(") OR key_id = 0)"));
        }
    }

    // Fetch closest-tempo candidates first, capped, so a huge library stays
    // bounded while keeping the tracks most likely to score well.
    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
                          "SELECT library.id, artist, title, album, bpm, key, "
                          "duration, rating, key_id "
                          "FROM library "
                          "INNER JOIN track_locations "
                          "ON library.location = track_locations.id "
                          "WHERE ") +
            conditions.join(QStringLiteral(" AND ")) +
            QStringLiteral(" ORDER BY ABS(bpm - :fromBpm) ASC LIMIT 1500"));
    query.bindValue(QStringLiteral(":fromId"), fromId);
    query.bindValue(QStringLiteral(":fromBpm"), fromBpm);
    if (fromBpm > 0.0) {
        query.bindValue(QStringLiteral(":bpmLo"), fromBpm - bpmWindow);
        query.bindValue(QStringLiteral(":bpmHi"), fromBpm + bpmWindow);
    }

    struct Ranked {
        double score;
        int id;
        QJsonObject row;
    };
    QVector<Ranked> ranked;
    if (query.exec()) {
        while (query.next()) {
            const int trackId = query.value(0).toInt();
            harmonic::Candidate candidate;
            candidate.fromBpm = fromBpm;
            candidate.fromKey = fromKey;
            candidate.toBpm = query.value(4).toDouble();
            candidate.toKey = static_cast<ChromaticKey>(query.value(8).toInt());
            candidate.playedTonight = playedIds.contains(trackId);
            candidate.rating = query.value(7).toInt();
            const harmonic::Score scored = harmonic::scoreCandidate(candidate);

            QJsonObject row;
            row.insert(QStringLiteral("id"), trackId);
            row.insert(QStringLiteral("artist"), query.value(1).toString());
            row.insert(QStringLiteral("title"), query.value(2).toString());
            const QString album = query.value(3).toString();
            if (!album.isEmpty()) {
                row.insert(QStringLiteral("album"), album);
            }
            if (candidate.toBpm > 0.0) {
                row.insert(QStringLiteral("bpm"), candidate.toBpm);
            }
            const QString keyText = query.value(5).toString();
            if (!keyText.isEmpty()) {
                row.insert(QStringLiteral("key"), keyText);
            }
            row.insert(QStringLiteral("durationSeconds"), query.value(6).toDouble());
            if (candidate.rating > 0) {
                row.insert(QStringLiteral("rating"), candidate.rating);
            }
            if (candidate.playedTonight) {
                row.insert(QStringLiteral("playedTonight"), true);
            }
            row.insert(QStringLiteral("score"),
                    std::round(scored.score * 10.0) / 10.0);
            QJsonArray reasons;
            for (const QString& reason : scored.reasons) {
                reasons.append(reason);
            }
            row.insert(QStringLiteral("reasons"), reasons);

            ranked.append(Ranked{scored.score, trackId, row});
        }
    } else {
        kLogger.warning() << "suggestions query failed:"
                          << query.lastError().text();
    }

    // Highest score first; ties broken by id for a stable order.
    std::stable_sort(ranked.begin(), ranked.end(),
            [](const Ranked& a, const Ranked& b) {
                if (a.score != b.score) {
                    return a.score > b.score;
                }
                return a.id < b.id;
            });

    QJsonArray suggestions;
    for (int i = 0; i < ranked.size() && i < limit; ++i) {
        suggestions.append(ranked.at(i).row);
    }

    QJsonObject fromTrack;
    fromTrack.insert(QStringLiteral("id"), fromId);
    fromTrack.insert(QStringLiteral("title"), pFrom->getTitle());
    fromTrack.insert(QStringLiteral("artist"), pFrom->getArtist());
    if (fromBpm > 0.0) {
        fromTrack.insert(QStringLiteral("bpm"), fromBpm);
    }
    const QString fromKeyText = pFrom->getKeyText();
    if (!fromKeyText.isEmpty()) {
        fromTrack.insert(QStringLiteral("key"), fromKeyText);
    }

    QJsonObject root;
    root.insert(QStringLiteral("deck"), deck);
    root.insert(QStringLiteral("fromTrack"), fromTrack);
    root.insert(QStringLiteral("suggestions"), suggestions);
    return compactJson(root);
}

QByteArray CompanionService::getTrackCover(int trackId) {
    // Main thread (Track + file access). Returns JPEG bytes or empty for 404.
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager) {
        return QByteArray();
    }
    const QVariant idVariant(trackId);
    const TrackId id(idVariant);
    if (!id.isValid()) {
        return QByteArray();
    }
    const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(id);
    if (!pTrack) {
        return QByteArray();
    }
    const CoverInfo coverInfo = pTrack->getCoverInfoWithLocation();
    const CoverInfo::LoadedImage loaded = coverInfo.loadImage(pTrack);
    if (loaded.result != CoverInfo::LoadedImage::Result::Ok ||
            loaded.image.isNull()) {
        return QByteArray();
    }
    QImage image = loaded.image;
    // Phones render thumbnails/HUD tiles; cap the size to keep transfers small.
    constexpr int kMaxEdgePx = 512;
    if (image.width() > kMaxEdgePx || image.height() > kMaxEdgePx) {
        image = image.scaled(kMaxEdgePx,
                kMaxEdgePx,
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 85);
    return jpeg;
}

void CompanionService::emitLibraryView() {
    QAbstractItemModel* pModel = m_pLibraryModel.data();
    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("library.view"));
    auto* pTrackModel = dynamic_cast<TrackModel*>(pModel);
    if (pTrackModel) {
        event.insert(QStringLiteral("viewKey"),
                pTrackModel->modelKey(/*noSearch*/ true));
        const QString search = pTrackModel->currentSearch();
        if (!search.isEmpty()) {
            event.insert(QStringLiteral("search"), search);
        }
    }
    event.insert(QStringLiteral("rowCount"), pModel ? pModel->rowCount() : 0);
    emit libraryViewEvent(event);
}

void CompanionService::onShowTrackModel(QAbstractItemModel* pModel) {
    // Main thread. The user switched the library to a new view (sidebar item,
    // search result, ...). Remember the model so cursor events can resolve rows.
    for (const QMetaObject::Connection& connection : m_libraryModelConnections) {
        disconnect(connection);
    }
    m_libraryModelConnections.clear();
    m_pLibraryModel = pModel;

    if (pModel) {
        // Searching, sorting, and add/remove all update the model IN PLACE
        // (model reset / row change) without a new showTrackModel — hook the
        // model itself so the phone's rowCount/window never go stale.
        m_libraryModelConnections << connect(pModel,
                &QAbstractItemModel::modelReset,
                this,
                &CompanionService::emitLibraryView);
        m_libraryModelConnections << connect(pModel,
                &QAbstractItemModel::rowsInserted,
                this,
                &CompanionService::emitLibraryView);
        m_libraryModelConnections << connect(pModel,
                &QAbstractItemModel::rowsRemoved,
                this,
                &CompanionService::emitLibraryView);
    }
    emitLibraryView();
}

void CompanionService::queueLibraryChanged(
        const char* field, const QSet<TrackId>& trackIds) {
    // Coalesce bursts (imports, batch analysis) into one event per 500 ms.
    constexpr int kMaxIdsPerEvent = 100;
    QJsonArray ids = m_pendingLibraryChanged
                             .value(QLatin1String(field))
                             .toArray();
    for (const TrackId& id : trackIds) {
        if (ids.size() >= kMaxIdsPerEvent) {
            break;
        }
        ids.append(id.toVariant().toInt());
    }
    m_pendingLibraryChanged.insert(QLatin1String(field), ids);
    if (m_pLibraryChangedTimer && !m_pLibraryChangedTimer->isActive()) {
        m_pLibraryChangedTimer->start();
    }
}

QJsonObject CompanionService::buildCursorWindow(
        TrackModel* pTrackModel, int cursorRow) const {
    // A small window of rows around the cursor so the phone renders the list
    // exactly as the user sees it, without mirroring the whole view.
    constexpr int kRowsBefore = 4;
    constexpr int kRowsAfter = 6;

    QAbstractItemModel* pModel = m_pLibraryModel.data();
    QJsonObject window;
    if (!pModel) {
        return window;
    }
    const int rowCount = pModel->rowCount();
    const int start = qBound(0, cursorRow - kRowsBefore, qMax(0, rowCount - 1));
    const int end = qMin(rowCount - 1, cursorRow + kRowsAfter);

    const int colTitle = pTrackModel->fieldIndex(QStringLiteral("title"));
    const int colArtist = pTrackModel->fieldIndex(QStringLiteral("artist"));
    const int colBpm = pTrackModel->fieldIndex(QStringLiteral("bpm"));
    const int colKey = pTrackModel->fieldIndex(QStringLiteral("key"));

    QJsonArray rows;
    for (int row = start; row <= end; ++row) {
        QJsonObject item;
        item.insert(QStringLiteral("row"), row);
        const TrackId id = pTrackModel->getTrackId(pModel->index(row, 0));
        if (id.isValid()) {
            item.insert(QStringLiteral("id"), id.toVariant().toInt());
        }
        if (colTitle >= 0) {
            item.insert(QStringLiteral("title"),
                    pModel->index(row, colTitle).data().toString());
        }
        if (colArtist >= 0) {
            item.insert(QStringLiteral("artist"),
                    pModel->index(row, colArtist).data().toString());
        }
        if (colBpm >= 0) {
            const double bpm = pModel->index(row, colBpm).data().toDouble();
            if (bpm > 0.0) {
                item.insert(QStringLiteral("bpm"), bpm);
            }
        }
        if (colKey >= 0) {
            const QString key = pModel->index(row, colKey).data().toString();
            if (!key.isEmpty()) {
                item.insert(QStringLiteral("key"), key);
            }
        }
        rows.append(item);
    }
    window.insert(QStringLiteral("start"), start);
    window.insert(QStringLiteral("rows"), rows);
    return window;
}

void CompanionService::onTrackSelected(const TrackPointer& pTrack) {
    // Main thread; debounced by WTrackTableView (~100 ms after the cursor
    // settles). A null track means the selection was cleared or is multi-row.
    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("library.cursor"));

    auto* pTrackModel = dynamic_cast<TrackModel*>(m_pLibraryModel.data());
    if (!pTrack || !pTrackModel) {
        event.insert(QStringLiteral("row"), -1);
        emit libraryCursorEvent(event);
        return;
    }

    const int cursorRow =
            pTrackModel->getTrackRows(pTrack->getId()).value(0, -1);
    event.insert(QStringLiteral("viewKey"),
            pTrackModel->modelKey(/*noSearch*/ true));
    event.insert(QStringLiteral("row"), cursorRow);
    event.insert(QStringLiteral("rowCount"),
            m_pLibraryModel ? m_pLibraryModel->rowCount() : 0);

    const CompanionSettings settings(m_pConfig);
    event.insert(QStringLiteral("track"),
            serializeTrack(pTrack, settings.exposeFilePaths()));
    if (cursorRow >= 0) {
        event.insert(QStringLiteral("window"),
                buildCursorWindow(pTrackModel, cursorRow));
    }
    emit libraryCursorEvent(event);
}

void CompanionService::onPersistTokens(const QString& tokensJson) {
    // Runs on the main thread (settings write). Flush to disk immediately so a
    // pairing survives an unclean exit, not only a graceful quit.
    CompanionSettings(m_pConfig).setPairedTokens(tokensJson);
    m_pConfig->save();
}

void CompanionService::onLoadToDeckRequested(int deck, int trackId, bool play) {
    // Runs on the main thread: DAO lookup + PlayerManager access are main-thread
    // only. Deck validity was checked on the server against numberOfDecks().
    VERIFY_OR_DEBUG_ASSERT(m_pTrackCollectionManager && m_pPlayerManager) {
        return;
    }
    const QVariant idVariant(trackId);
    const TrackId id(idVariant);
    if (!id.isValid()) {
        kLogger.warning() << "load: invalid track id" << trackId;
        return;
    }
    const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(id);
    if (!pTrack) {
        kLogger.warning() << "load: no track with id" << trackId;
        return;
    }
    m_pPlayerManager->slotLoadLocationToPlayer(
            pTrack->getLocation(), PlayerManager::groupForDeck(deck - 1), play);
    kLogger.debug() << "load: track" << trackId << "-> deck" << deck;
}

} // namespace companion
} // namespace mixxx

#include "moc_companionservice.cpp"
