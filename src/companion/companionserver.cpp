#include "companion/companionserver.h"

#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QTcpServer>
#include <QUrlQuery>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

#include "companion/companiondefs.h"
#include "companion/deckstatepublisher.h"
#include "companion/httpconnection.h"
#include "control/controlobject.h"
#include "mixer/playermanager.h"
#include "preferences/configobject.h"
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
        QObject* pQueryHandler,
        QObject* parent)
        : QObject(parent),
          m_bindAddress(std::move(bindAddress)),
          m_port(port),
          m_tickIntervalMs(tickIntervalMs),
          m_appVersion(std::move(appVersion)),
          m_numDecks(numDecks),
          m_pQueryHandler(pQueryHandler),
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
    if (request.method == "GET" &&
            request.path == QLatin1String("/v1/library/search")) {
        return handleSearch(request);
    }
    if (request.method == "GET" &&
            request.path == QLatin1String("/v1/decks")) {
        return handleDecks();
    }

    const QStringList segments =
            request.path.split('/', Qt::SkipEmptyParts);

    // GET /v1/decks/:deck
    if (request.method == "GET" && segments.size() == 3 &&
            segments.at(0) == QLatin1String("v1") &&
            segments.at(1) == QLatin1String("decks")) {
        bool deckOk = false;
        const int deck = segments.at(2).toInt(&deckOk);
        if (!deckOk) {
            return HttpResponse::error(400, "bad_request", "invalid deck");
        }
        if (!isValidDeck(deck)) {
            return HttpResponse::error(404, "not_found", "no such deck");
        }
        return HttpResponse::json(200,
                QJsonDocument(deckStateJson(deck)).toJson(QJsonDocument::Compact));
    }

    // GET /v1/tracks/:id  |  /v1/tracks/:id/cues  |  /v1/tracks/:id/waveform/summary
    if (request.method == "GET" && segments.size() >= 3 &&
            segments.at(0) == QLatin1String("v1") &&
            segments.at(1) == QLatin1String("tracks")) {
        bool idOk = false;
        const int trackId = segments.at(2).toInt(&idOk);
        if (!idOk) {
            return HttpResponse::error(400, "bad_request", "invalid track id");
        }
        if (segments.size() == 3) {
            return handleTrack(trackId);
        }
        if (segments.size() == 4 && segments.at(3) == QLatin1String("cues")) {
            return handleTrackCues(trackId);
        }
        if (segments.size() == 5 &&
                segments.at(3) == QLatin1String("waveform") &&
                segments.at(4) == QLatin1String("summary")) {
            return handleWaveformSummary(trackId);
        }
        return HttpResponse::error(404, "not_found");
    }

    // POST /v1/decks/:deck/:action
    if (request.method == "POST" && segments.size() == 4 &&
            segments.at(0) == QLatin1String("v1") &&
            segments.at(1) == QLatin1String("decks")) {
        bool deckOk = false;
        const int deck = segments.at(2).toInt(&deckOk);
        if (!deckOk) {
            return HttpResponse::error(400, "bad_request", "invalid deck");
        }
        if (!isValidDeck(deck)) {
            return HttpResponse::error(404, "not_found", "no such deck");
        }
        return handleDeckAction(deck, segments.at(3).toUtf8(), request);
    }

    return HttpResponse::error(404, "not_found");
}

bool CompanionServer::isValidDeck(int deck) const {
    return deck >= 1 && deck <= m_numDecks;
}

QJsonObject CompanionServer::deckStateJson(int deck) const {
    QJsonObject dto;
    dto.insert(QStringLiteral("deck"), deck);
    const auto it = m_deckSnapshots.constFind(deck);
    if (it == m_deckSnapshots.constEnd()) {
        dto.insert(QStringLiteral("generation"), 0);
        dto.insert(QStringLiteral("playing"), false);
        return dto;
    }
    dto.insert(QStringLiteral("generation"), static_cast<qint64>(it->generation));
    if (it->loaded && it->loadedEvent.contains(QStringLiteral("track"))) {
        dto.insert(QStringLiteral("track"),
                it->loadedEvent.value(QStringLiteral("track")));
    }
    // Merge the latest tick fields (present within one tick interval of load).
    const QJsonObject& tick = it->lastTick;
    dto.insert(QStringLiteral("playposition"),
            tick.value(QStringLiteral("playposition")).toDouble(0.0));
    dto.insert(QStringLiteral("positionSeconds"),
            tick.value(QStringLiteral("positionSeconds")).toDouble(0.0));
    dto.insert(QStringLiteral("durationSeconds"),
            tick.value(QStringLiteral("durationSeconds")).toDouble(0.0));
    dto.insert(QStringLiteral("rate"),
            tick.value(QStringLiteral("rate")).toDouble(0.0));
    dto.insert(QStringLiteral("playing"),
            tick.value(QStringLiteral("playing")).toBool(false));
    dto.insert(QStringLiteral("vu"), tick.value(QStringLiteral("vu")).toDouble(0.0));
    return dto;
}

HttpResponse CompanionServer::handleDecks() {
    QJsonArray decks;
    for (int deck = 1; deck <= m_numDecks; ++deck) {
        decks.append(deckStateJson(deck));
    }
    return HttpResponse::json(
            200, QJsonDocument(decks).toJson(QJsonDocument::Compact));
}

HttpResponse CompanionServer::handleTrack(int trackId) {
    if (!m_pQueryHandler) {
        return HttpResponse::error(500, "internal", "no query handler");
    }
    QByteArray json;
    const bool ok = QMetaObject::invokeMethod(m_pQueryHandler,
            "getTrackJson",
            Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(QByteArray, json),
            Q_ARG(int, trackId));
    if (!ok) {
        return HttpResponse::error(500, "internal", "track lookup failed");
    }
    if (json.isEmpty()) {
        return HttpResponse::error(404, "not_found", "no such track");
    }
    return HttpResponse::json(200, json);
}

HttpResponse CompanionServer::handleTrackCues(int trackId) {
    if (!m_pQueryHandler) {
        return HttpResponse::error(500, "internal", "no query handler");
    }
    QByteArray json;
    const bool ok = QMetaObject::invokeMethod(m_pQueryHandler,
            "getTrackCues",
            Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(QByteArray, json),
            Q_ARG(int, trackId));
    if (!ok) {
        return HttpResponse::error(500, "internal", "cue lookup failed");
    }
    if (json.isEmpty()) {
        return HttpResponse::error(404, "not_found", "no such track");
    }
    return HttpResponse::json(200, json);
}

HttpResponse CompanionServer::handleWaveformSummary(int trackId) {
    if (!m_pQueryHandler) {
        return HttpResponse::error(500, "internal", "no query handler");
    }
    QByteArray blob;
    const bool ok = QMetaObject::invokeMethod(m_pQueryHandler,
            "exportWaveformSummary",
            Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(QByteArray, blob),
            Q_ARG(int, trackId));
    if (!ok) {
        return HttpResponse::error(500, "internal", "waveform export failed");
    }
    if (blob.isEmpty()) {
        // No completed summary waveform available (track unknown or still being
        // analyzed). The client retries after the next deck.loaded or a backoff.
        return HttpResponse::error(202, "analysis_pending");
    }
    HttpResponse response;
    response.status = 200;
    response.contentType = "application/octet-stream";
    response.body = blob;
    return response;
}

HttpResponse CompanionServer::handleSearch(const HttpRequest& request) {
    if (!m_pQueryHandler) {
        return HttpResponse::error(500, "internal", "no query handler");
    }
    const QUrlQuery& query = request.query;
    const QString q = query.queryItemValue(QStringLiteral("q"));
    const int bpmMin = query.queryItemValue(QStringLiteral("bpmMin")).toInt();
    const int bpmMax = query.queryItemValue(QStringLiteral("bpmMax")).toInt();
    const QString key = query.queryItemValue(QStringLiteral("key"));
    int limit = 50;
    if (query.hasQueryItem(QStringLiteral("limit"))) {
        limit = query.queryItemValue(QStringLiteral("limit")).toInt();
    }
    limit = qBound(1, limit, 200);
    int offset = query.queryItemValue(QStringLiteral("offset")).toInt();
    if (offset < 0) {
        offset = 0;
    }

    // Runs on the main thread (DB access); blocks this worker HTTP handler until
    // it returns. CompanionService::stop() spins an event loop so this can never
    // deadlock teardown.
    QByteArray json;
    const bool ok = QMetaObject::invokeMethod(m_pQueryHandler,
            "runLibrarySearch",
            Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(QByteArray, json),
            Q_ARG(QString, q),
            Q_ARG(int, bpmMin),
            Q_ARG(int, bpmMax),
            Q_ARG(QString, key),
            Q_ARG(int, limit),
            Q_ARG(int, offset));
    if (!ok) {
        return HttpResponse::error(500, "internal", "search failed");
    }
    return HttpResponse::json(200, json);
}

HttpResponse CompanionServer::handleDeckAction(
        int deck, const QByteArray& action, const HttpRequest& request) {
    const QString group = PlayerManager::groupForDeck(deck - 1);

    // Transport controls are thread-safe via the control system and run
    // directly on this (worker) thread.
    if (action == "play") {
        ControlObject::set(ConfigKey(group, QStringLiteral("play")), 1.0);
        return HttpResponse::json(200, "{\"ok\":true}");
    }
    if (action == "pause") {
        ControlObject::set(ConfigKey(group, QStringLiteral("play")), 0.0);
        return HttpResponse::json(200, "{\"ok\":true}");
    }
    if (action == "cue") {
        // Momentary press+release; in the default cue mode this jumps to the
        // cue point.
        ControlObject::set(ConfigKey(group, QStringLiteral("cue_default")), 1.0);
        ControlObject::set(ConfigKey(group, QStringLiteral("cue_default")), 0.0);
        return HttpResponse::json(200, "{\"ok\":true}");
    }
    if (action == "sync") {
        ControlObject::set(ConfigKey(group, QStringLiteral("beatsync")), 1.0);
        return HttpResponse::json(200, "{\"ok\":true}");
    }
    if (action == "seek") {
        const QJsonObject body =
                QJsonDocument::fromJson(request.body).object();
        if (!body.contains(QStringLiteral("position"))) {
            return HttpResponse::error(400, "bad_request", "position required");
        }
        const double position = body.value(QStringLiteral("position")).toDouble();
        if (position < 0.0 || position > 1.0) {
            return HttpResponse::error(
                    400, "bad_request", "position must be 0..1");
        }
        const bool playing =
                ControlObject::get(ConfigKey(group, QStringLiteral("play"))) != 0.0;
        const bool force = body.value(QStringLiteral("force")).toBool();
        if (playing && !force) {
            return HttpResponse::error(
                    409, "action_not_allowed", "deck is playing; pass force");
        }
        ControlObject::set(
                ConfigKey(group, QStringLiteral("playposition")), position);
        return HttpResponse::json(200, "{\"ok\":true}");
    }
    if (action == "load") {
        const QJsonObject body =
                QJsonDocument::fromJson(request.body).object();
        if (!body.contains(QStringLiteral("trackId"))) {
            return HttpResponse::error(400, "bad_request", "trackId required");
        }
        const int trackId = body.value(QStringLiteral("trackId")).toInt();
        const bool play = body.value(QStringLiteral("play")).toBool();
        // Track lookup + load happen on the main thread; fire-and-forget. The
        // resulting deck.loaded event confirms success to the client.
        emit loadToDeckRequested(deck, trackId, play);
        return HttpResponse::json(202, "{\"ok\":true,\"status\":\"loading\"}");
    }

    return HttpResponse::error(403, "action_not_allowed", "unknown action");
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
