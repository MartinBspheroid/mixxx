#include "companion/companionservice.h"

#include <QEventLoop>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <QDataStream>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include "companion/companionserver.h"
#include "companion/companionsettings.h"
#include "companion/trackserializer.h"
#include "companion/waveformmxwf.h"
#include "library/dao/analysisdao.h"
#include "library/searchquery.h"
#include "library/searchqueryparser.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "track/cue.h"
#include "track/track.h"
#include "track/trackid.h"
#include "util/assert.h"
#include "util/color/rgbcolor.h"
#include "util/logger.h"
#include "waveform/waveform.h"
#include "waveform/waveformfactory.h"

namespace {
QString cueTypeString(mixxx::CueType type) {
    switch (type) {
    case mixxx::CueType::HotCue:
        return QStringLiteral("hotcue");
    case mixxx::CueType::MainCue:
        return QStringLiteral("maincue");
    case mixxx::CueType::Loop:
        return QStringLiteral("loop");
    case mixxx::CueType::Intro:
        return QStringLiteral("intro");
    case mixxx::CueType::Outro:
        return QStringLiteral("outro");
    default:
        // Invalid/Beat/Jump/N60dBSound are not exposed to companion clients.
        return QString();
    }
}
} // namespace

namespace {
const mixxx::Logger kLogger("Companion");
} // namespace

namespace mixxx {
namespace companion {

CompanionService::CompanionService(UserSettingsPointer pConfig,
        PlayerManager* pPlayerManager,
        TrackCollectionManager* pTrackCollectionManager,
        QString appVersion,
        QObject* parent)
        : QObject(parent),
          m_pConfig(std::move(pConfig)),
          m_pPlayerManager(pPlayerManager),
          m_pTrackCollectionManager(pTrackCollectionManager),
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
            settings.pairedTokens());
    m_pServer->moveToThread(m_pThread);

    connect(this,
            &CompanionService::deckLoadedEvent,
            m_pServer,
            &CompanionServer::onDeckLoaded);
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
            &CompanionServer::persistTokens,
            this,
            &CompanionService::onPersistTokens);

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
    kLogger.info() << "Companion API stopped";
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
}

void CompanionService::onDeckUnloaded(int deckIndex) {
    const int deck = deckIndex + 1;
    const quint64 generation = ++m_generations[deck];
    emit deckUnloadedEvent(deck, generation);
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
            row.insert(QStringLiteral("id"), query.value(0).toInt());
            row.insert(QStringLiteral("title"), query.value(2).toString());
            row.insert(QStringLiteral("artist"), query.value(1).toString());
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
    const double sampleRate = pTrack->getSampleRate().value();

    QJsonArray cues;
    const QList<CuePointer> cuePoints = pTrack->getCuePoints();
    for (const CuePointer& pCue : cuePoints) {
        if (!pCue) {
            continue;
        }
        const QString type = cueTypeString(pCue->getType());
        if (type.isEmpty()) {
            continue;
        }
        QJsonObject cue;
        cue.insert(QStringLiteral("type"), type);
        const mixxx::audio::FramePos position = pCue->getPosition();
        if (position.isValid() && sampleRate > 0.0) {
            cue.insert(QStringLiteral("positionSeconds"),
                    position.value() / sampleRate);
            const mixxx::audio::FramePos endPosition = pCue->getEndPosition();
            if (endPosition.isValid()) {
                cue.insert(QStringLiteral("lengthSeconds"),
                        (endPosition.value() - position.value()) / sampleRate);
            }
        }
        const int hotcue = pCue->getHotCue();
        if (hotcue >= 0) {
            cue.insert(QStringLiteral("index"), hotcue);
        }
        const QString label = pCue->getLabel();
        if (!label.isEmpty()) {
            cue.insert(QStringLiteral("label"), label);
        }
        cue.insert(QStringLiteral("color"),
                mixxx::RgbColor::toQString(pCue->getColor()));
        cues.append(cue);
    }

    QJsonObject root;
    root.insert(QStringLiteral("trackId"), trackId);
    root.insert(QStringLiteral("cues"), cues);
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
