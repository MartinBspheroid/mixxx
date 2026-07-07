#include "companion/companionservice.h"

#include <QMetaObject>
#include <QThread>

#include "companion/companionserver.h"
#include "companion/companionsettings.h"
#include "companion/trackserializer.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "track/track.h"
#include "track/trackid.h"
#include "util/assert.h"
#include "util/logger.h"

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
            numDecks);
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
        // Shut down the server on its own thread, then join.
        QMetaObject::invokeMethod(
                m_pServer, "shutdown", Qt::BlockingQueuedConnection);
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
