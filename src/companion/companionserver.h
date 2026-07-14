#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include "companion/pairingmanager.h"

class QTcpServer;
class QTcpSocket;
class QWebSocket;
class QWebSocketServer;

namespace mixxx {
namespace companion {

class DeckStatePublisher;
struct HttpRequest;
struct HttpResponse;

/// Owns the TCP listener (HTTP), the WebSocket server, the connected-client
/// registry and the per-deck state snapshots. Serves `GET /v1/status` and the
/// `/ws/v1` event stream.
///
/// Constructed on the main thread, then moved to the Companion worker thread;
/// all sockets, timers and the DeckStatePublisher it owns live on that thread.
/// Cross-thread entry points (onDeckLoaded/onDeckUnloaded/etc.) are invoked via
/// queued connections from CompanionService.
class CompanionServer : public QObject {
    Q_OBJECT
  public:
    /// @param pQueryHandler main-thread object exposing the invokable
    ///        `QByteArray runLibrarySearch(...)` slot. Called via
    ///        BlockingQueuedConnection for request/response endpoints that need
    ///        main-thread DB access. Must outlive this server.
    CompanionServer(QHostAddress bindAddress,
            quint16 port,
            int tickIntervalMs,
            QString appVersion,
            int numDecks,
            QObject* pQueryHandler,
            const QString& pairedTokensJson,
            const QString& sessionCode,
            QObject* parent = nullptr);
    ~CompanionServer() override;

  public slots:
    /// Create the listeners and start accepting connections. Runs on the worker
    /// thread (invoked via queued connection once the thread's event loop runs).
    void initialize();
    /// Stop accepting connections, disconnect clients and free listeners.
    void shutdown();
    /// Replace the session pairing code (invoked from the main thread when the
    /// user regenerates it in Preferences).
    void setSessionCode(const QString& code);

    /// A deck loaded a track. `track` is the already-serialized TrackDto, built
    /// on the main thread; `generation` is the deck's load counter.
    void onDeckLoaded(int deck, quint64 generation, const QJsonObject& track);
    /// A deck's beat grid became available or changed (analysis finished, tap
    /// BPM, manual adjust). `beatgrid` is the serialized BeatgridDto; events
    /// whose `generation` is not the deck's current one are stale and dropped.
    void onDeckBeatgrid(
            int deck, quint64 generation, const QJsonObject& beatgrid);
    /// A deck was emptied.
    void onDeckUnloaded(int deck, quint64 generation);
    /// The number of decks changed (rewires the publisher).
    void onNumberOfDecksChanged(int numDecks);
    /// The library view changed (new folder/crate/playlist shown). Snapshot is
    /// stored for replay and broadcast as `library.view`.
    void onLibraryView(const QJsonObject& event);
    /// The library cursor (highlighted track) changed. Broadcast as
    /// `library.cursor`.
    void onLibraryCursor(const QJsonObject& event);
    /// Coarse library invalidation (tracks added/changed/removed). Broadcast
    /// as `library.changed`, not replayed.
    void onLibraryChanged(const QJsonObject& event);

  signals:
    /// Emitted after a successful listen(), for logging/status.
    void listening(quint16 port);
    /// Emitted after shutdown() completes.
    void stopped();
    /// A client asked to load a library track to a deck. Handled on the main
    /// thread by CompanionService (track lookup + PlayerManager).
    void loadToDeckRequested(int deck, int trackId, bool play);
    /// A client asked to append a library track to the Auto DJ queue.
    void autoDjQueueRequested(int trackId);
    /// The paired-token set changed; the JSON should be persisted to settings on
    /// the main thread.
    void persistTokens(const QString& tokensJson);

  private slots:
    void onNewConnection();
    void onWebSocketUpgradeRequested(QTcpSocket* pSocket,
            const QByteArray& token,
            bool fromLoopback,
            const QString& peerAddress);
    void onWebSocketConnection();
    void onClientDisconnected();
    void onClientTextMessage(const QString& message);
    /// A tick from the publisher, missing only generation + serverTimeMs.
    void onTickReady(int deck, const QJsonObject& partialTick);
    /// The deck configuration (engine/visible deck count) changed.
    void onDecksConfigChanged(int numDecks, int visibleDecks);

  private:
    /// Per-IP throttle on failed auth: returns true while the peer is locked
    /// out. Records happen via recordAuthFailure/clearAuthFailures.
    bool isAuthThrottled(const QString& peer, qint64 nowMs);
    void recordAuthFailure(const QString& peer, qint64 nowMs);
    void clearAuthFailures(const QString& peer);

    /// Broadcast an event to all clients. Events marked droppable (ticks,
    /// cursor updates) are skipped for clients whose send buffer is backed up;
    /// any client past the hard cap is disconnected.
    void broadcast(const QJsonObject& event, bool droppable);

    HttpResponse route(const HttpRequest& request);
    HttpResponse handlePairing(const QStringList& segments, const HttpRequest& request);
    HttpResponse handleLibraryNav(
            const QString& action, const HttpRequest& request);
    HttpResponse handleStatus();
    HttpResponse handleDecks();
    QJsonObject deckStateJson(int deck) const;
    HttpResponse handleTrack(int trackId);
    HttpResponse handleTrackCues(int trackId);
    HttpResponse handleWaveformSummary(int trackId);
    HttpResponse handleSearch(const HttpRequest& request);
    HttpResponse handleDeckAction(
            int deck, const QByteArray& action, const HttpRequest& request);
    bool isValidDeck(int deck) const;
    void sendReplay(QWebSocket* pClient);
    qint64 serverTimeMs() const;

    const QHostAddress m_bindAddress;
    const quint16 m_port;
    const int m_tickIntervalMs;
    const QString m_appVersion;
    int m_numDecks;
    int m_visibleDecks;
    QObject* m_pQueryHandler;
    QJsonObject m_lastLibraryView;   ///< latest library.view, for replay
    QJsonObject m_lastLibraryCursor; ///< latest library.cursor, for replay

    struct AuthFailures {
        int count = 0;
        qint64 lockedUntilMs = 0;
        qint64 lastFailureMs = 0;
    };
    QHash<QString, AuthFailures> m_authFailures; ///< keyed by peer IP
    int m_activeHttpConnections = 0;

    PairingManager m_pairing;

    QTcpServer* m_pTcpServer;
    QWebSocketServer* m_pWsServer;
    DeckStatePublisher* m_pPublisher;
    QList<QWebSocket*> m_clients;
    /// Topic subscriptions per client; default is {"decks"} (spec). Events are
    /// routed by topic: library.* -> "library", everything else -> "decks".
    QHash<QWebSocket*, QSet<QString>> m_clientTopics;
    void sendLibraryReplay(QWebSocket* pClient);
    QElapsedTimer m_uptime;

    struct DeckSnapshot {
        bool loaded = false;
        quint64 generation = 0;
        QJsonObject loadedEvent;   ///< last deck.loaded event, for replay
        QJsonObject beatgridEvent; ///< last deck.beatgrid event, for replay
        QJsonObject lastTick;      ///< last deck.tick event, for replay
    };
    QHash<int, DeckSnapshot> m_deckSnapshots;
};

} // namespace companion
} // namespace mixxx
