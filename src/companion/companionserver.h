#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

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
    CompanionServer(QHostAddress bindAddress,
            quint16 port,
            int tickIntervalMs,
            QString appVersion,
            int numDecks,
            QObject* parent = nullptr);
    ~CompanionServer() override;

  public slots:
    /// Create the listeners and start accepting connections. Runs on the worker
    /// thread (invoked via queued connection once the thread's event loop runs).
    void initialize();
    /// Stop accepting connections, disconnect clients and free listeners.
    void shutdown();

    /// A deck loaded a track. `track` is the already-serialized TrackDto, built
    /// on the main thread; `generation` is the deck's load counter.
    void onDeckLoaded(int deck, quint64 generation, const QJsonObject& track);
    /// A deck was emptied.
    void onDeckUnloaded(int deck, quint64 generation);
    /// The number of decks changed (rewires the publisher).
    void onNumberOfDecksChanged(int numDecks);

  signals:
    /// Emitted after a successful listen(), for logging/status.
    void listening(quint16 port);
    /// Emitted after shutdown() completes.
    void stopped();

  private slots:
    void onNewConnection();
    void onWebSocketUpgradeRequested(QTcpSocket* pSocket);
    void onWebSocketConnection();
    void onClientDisconnected();
    void onClientTextMessage(const QString& message);
    /// A tick from the publisher, missing only generation + serverTimeMs.
    void onTickReady(int deck, const QJsonObject& partialTick);

  private:
    HttpResponse route(const HttpRequest& request);
    HttpResponse handleStatus();
    void sendReplay(QWebSocket* pClient);
    void broadcast(const QJsonObject& event);
    qint64 serverTimeMs() const;

    const QHostAddress m_bindAddress;
    const quint16 m_port;
    const int m_tickIntervalMs;
    const QString m_appVersion;
    int m_numDecks;

    QTcpServer* m_pTcpServer;
    QWebSocketServer* m_pWsServer;
    DeckStatePublisher* m_pPublisher;
    QList<QWebSocket*> m_clients;
    QElapsedTimer m_uptime;

    struct DeckSnapshot {
        bool loaded = false;
        quint64 generation = 0;
        QJsonObject loadedEvent; ///< last deck.loaded event, for replay
        QJsonObject lastTick;    ///< last deck.tick event, for replay
    };
    QHash<int, DeckSnapshot> m_deckSnapshots;
};

} // namespace companion
} // namespace mixxx
