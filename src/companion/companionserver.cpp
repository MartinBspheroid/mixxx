#include "companion/companionserver.h"

#include <utility>

#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

#include "companion/companiondefs.h"
#include "companion/deckstatepublisher.h"
#include "companion/httpconnection.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("Companion");

// Whole-request cap for the HTTP layer. Our request bodies are tiny JSON
// objects; anything larger is rejected rather than buffered.
constexpr qint64 kMaxRequestBytes = 64 * 1024;
} // namespace

namespace mixxx {
namespace companion {

CompanionServer::CompanionServer(QHostAddress bindAddress,
        quint16 port,
        int tickIntervalMs,
        QString appVersion,
        int numDecks,
        QObject* parent)
        : QObject(parent),
          m_bindAddress(std::move(bindAddress)),
          m_port(port),
          m_tickIntervalMs(tickIntervalMs),
          m_appVersion(std::move(appVersion)),
          m_numDecks(numDecks),
          m_pTcpServer(nullptr),
          m_pWsServer(nullptr),
          m_pPublisher(nullptr) {
}

CompanionServer::~CompanionServer() = default;

void CompanionServer::initialize() {
    m_uptime.start();

    m_pTcpServer = new QTcpServer(this);
    connect(m_pTcpServer,
            &QTcpServer::newConnection,
            this,
            &CompanionServer::onNewConnection);
    if (!m_pTcpServer->listen(m_bindAddress, m_port)) {
        kLogger.warning() << "Failed to listen on" << m_bindAddress.toString()
                          << "port" << m_port << ":"
                          << m_pTcpServer->errorString()
                          << "- Companion API is not available";
        return;
    }

    m_pWsServer = new QWebSocketServer(
            QStringLiteral("Mixxx Companion"),
            QWebSocketServer::NonSecureMode,
            this);
    connect(m_pWsServer,
            &QWebSocketServer::newConnection,
            this,
            &CompanionServer::onWebSocketConnection);

    m_pPublisher = new DeckStatePublisher(m_tickIntervalMs, this);
    connect(m_pPublisher,
            &DeckStatePublisher::tickReady,
            this,
            &CompanionServer::onTickReady);
    m_pPublisher->setDecks(m_numDecks);
    m_pPublisher->start();

    kLogger.info() << "Companion API listening on" << m_bindAddress.toString()
                   << "port" << m_port;
    emit listening(m_port);
}

void CompanionServer::shutdown() {
    // Runs on the worker thread: delete every thread-affine child (timer,
    // sockets, servers) here so the CompanionServer shell can then be destroyed
    // safely from the main thread after the thread has joined.
    if (m_pPublisher) {
        m_pPublisher->stop();
        delete m_pPublisher;
        m_pPublisher = nullptr;
    }
    if (m_pTcpServer) {
        m_pTcpServer->close();
        delete m_pTcpServer;
        m_pTcpServer = nullptr;
    }
    for (QWebSocket* pClient : std::as_const(m_clients)) {
        disconnect(pClient, nullptr, this, nullptr);
        pClient->close();
    }
    m_clients.clear();
    if (m_pWsServer) {
        // Deletes any QWebSockets it still parents (including m_clients above).
        m_pWsServer->close();
        delete m_pWsServer;
        m_pWsServer = nullptr;
    }
    emit stopped();
}

void CompanionServer::onNewConnection() {
    while (m_pTcpServer && m_pTcpServer->hasPendingConnections()) {
        QTcpSocket* pSocket = m_pTcpServer->nextPendingConnection();
        auto* pConnection = new HttpConnection(pSocket, kMaxRequestBytes, this);
        pConnection->setRouter(
                [this](const HttpRequest& request) { return route(request); });
        connect(pConnection,
                &HttpConnection::webSocketUpgradeRequested,
                this,
                &CompanionServer::onWebSocketUpgradeRequested);
    }
}

void CompanionServer::onWebSocketUpgradeRequested(QTcpSocket* pSocket) {
    if (m_pWsServer) {
        m_pWsServer->handleConnection(pSocket);
    } else {
        pSocket->deleteLater();
    }
}

void CompanionServer::onWebSocketConnection() {
    while (m_pWsServer && m_pWsServer->hasPendingConnections()) {
        QWebSocket* pClient = m_pWsServer->nextPendingConnection();
        m_clients.append(pClient);
        connect(pClient,
                &QWebSocket::textMessageReceived,
                this,
                &CompanionServer::onClientTextMessage);
        connect(pClient,
                &QWebSocket::disconnected,
                this,
                &CompanionServer::onClientDisconnected);
        kLogger.debug() << "Companion client connected;" << m_clients.size()
                        << "total";
        sendReplay(pClient);
    }
}

void CompanionServer::onClientDisconnected() {
    auto* pClient = qobject_cast<QWebSocket*>(sender());
    if (!pClient) {
        return;
    }
    m_clients.removeAll(pClient);
    pClient->deleteLater();
    kLogger.debug() << "Companion client disconnected;" << m_clients.size()
                    << "remaining";
}

void CompanionServer::onClientTextMessage(const QString& message) {
    auto* pClient = qobject_cast<QWebSocket*>(sender());
    if (!pClient) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("ping")) {
        QJsonObject pong;
        pong.insert(QStringLiteral("type"), QStringLiteral("pong"));
        pong.insert(QStringLiteral("t"), obj.value(QStringLiteral("t")));
        pClient->sendTextMessage(
                QString::fromUtf8(QJsonDocument(pong).toJson(QJsonDocument::Compact)));
    }
    // "hello" and "subscribe" are accepted but need no action in v1: every
    // client receives every deck event.
}

void CompanionServer::onTickReady(int deck, const QJsonObject& partialTick) {
    auto it = m_deckSnapshots.find(deck);
    if (it == m_deckSnapshots.end() || !it->loaded) {
        return; // stale: deck not currently loaded
    }
    QJsonObject event = partialTick;
    event.insert(QStringLiteral("generation"),
            static_cast<qint64>(it->generation));
    event.insert(QStringLiteral("serverTimeMs"), serverTimeMs());
    it->lastTick = event;
    broadcast(event);
}

void CompanionServer::onDeckLoaded(
        int deck, quint64 generation, const QJsonObject& track) {
    QJsonObject waveform;
    waveform.insert(QStringLiteral("summaryUrl"),
            QStringLiteral("/v1/tracks/%1/waveform/summary")
                    .arg(track.value(QStringLiteral("id")).toVariant().toString()));

    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("deck.loaded"));
    event.insert(QStringLiteral("deck"), deck);
    event.insert(QStringLiteral("generation"), static_cast<qint64>(generation));
    event.insert(QStringLiteral("track"), track);
    event.insert(QStringLiteral("waveform"), waveform);
    event.insert(QStringLiteral("serverTimeMs"), serverTimeMs());

    DeckSnapshot& snapshot = m_deckSnapshots[deck];
    snapshot.loaded = true;
    snapshot.generation = generation;
    snapshot.loadedEvent = event;
    snapshot.lastTick = QJsonObject();
    broadcast(event);
}

void CompanionServer::onDeckUnloaded(int deck, quint64 generation) {
    DeckSnapshot& snapshot = m_deckSnapshots[deck];
    snapshot.loaded = false;
    snapshot.generation = generation;
    snapshot.loadedEvent = QJsonObject();
    snapshot.lastTick = QJsonObject();

    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("deck.unloaded"));
    event.insert(QStringLiteral("deck"), deck);
    event.insert(QStringLiteral("generation"), static_cast<qint64>(generation));
    broadcast(event);
}

void CompanionServer::onNumberOfDecksChanged(int numDecks) {
    m_numDecks = numDecks;
    if (m_pPublisher) {
        m_pPublisher->setDecks(numDecks);
    }
}

void CompanionServer::sendReplay(QWebSocket* pClient) {
    for (auto it = m_deckSnapshots.constBegin(); it != m_deckSnapshots.constEnd();
            ++it) {
        if (!it->loaded) {
            continue;
        }
        pClient->sendTextMessage(QString::fromUtf8(
                QJsonDocument(it->loadedEvent).toJson(QJsonDocument::Compact)));
        if (!it->lastTick.isEmpty()) {
            pClient->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(it->lastTick).toJson(QJsonDocument::Compact)));
        }
    }
}

void CompanionServer::broadcast(const QJsonObject& event) {
    if (m_clients.isEmpty()) {
        return;
    }
    const QString payload = QString::fromUtf8(
            QJsonDocument(event).toJson(QJsonDocument::Compact));
    for (QWebSocket* pClient : std::as_const(m_clients)) {
        pClient->sendTextMessage(payload);
    }
}

qint64 CompanionServer::serverTimeMs() const {
    return m_uptime.isValid() ? m_uptime.elapsed() : 0;
}

HttpResponse CompanionServer::route(const HttpRequest& request) {
    if (request.method == "GET" && request.path == QLatin1String("/v1/status")) {
        return handleStatus();
    }
    return HttpResponse::error(404, "not_found");
}

HttpResponse CompanionServer::handleStatus() {
    QJsonObject status;
    status.insert(QStringLiteral("app"), QStringLiteral("Mixxx"));
    status.insert(QStringLiteral("version"), m_appVersion);
    status.insert(QStringLiteral("apiVersion"), kApiVersion);
    status.insert(QStringLiteral("numDecks"), m_numDecks);
    status.insert(QStringLiteral("libraryReady"), true);
    status.insert(QStringLiteral("uptimeMs"), serverTimeMs());
    status.insert(QStringLiteral("clients"), m_clients.size());
    status.insert(QStringLiteral("auth"),
            m_bindAddress == QHostAddress(QHostAddress::LocalHost)
                    ? QStringLiteral("open-loopback")
                    : QStringLiteral("open-lan"));
    return HttpResponse::json(
            200, QJsonDocument(status).toJson(QJsonDocument::Compact));
}

} // namespace companion
} // namespace mixxx

#include "moc_companionserver.cpp"
