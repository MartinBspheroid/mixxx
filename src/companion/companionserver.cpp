#include "companion/companionserver.h"

#include <utility>

#include <QDateTime>
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

// Concurrent HTTP connection cap (FD-exhaustion guard).
constexpr int kMaxHttpConnections = 64;

// Auth brute-force guard: after this many failures a peer is locked out, with
// the lockout doubling on continued failures. Failure history expires.
constexpr int kAuthFailuresBeforeLockout = 5;
constexpr qint64 kAuthLockoutBaseMs = 30 * 1000;
constexpr qint64 kAuthFailureExpiryMs = 15 * 60 * 1000;
constexpr int kMaxTrackedPeers = 1024;

// WebSocket backpressure: past the soft cap droppable events (ticks, cursor)
// are skipped for that client; past the hard cap the client is disconnected.
constexpr qint64 kWsSoftBufferCap = 256 * 1024;
constexpr qint64 kWsHardBufferCap = 2 * 1024 * 1024;
} // namespace

namespace mixxx {
namespace companion {

CompanionServer::CompanionServer(QHostAddress bindAddress,
        quint16 port,
        int tickIntervalMs,
        QString appVersion,
        int numDecks,
        QObject* pQueryHandler,
        const QString& pairedTokensJson,
        const QString& sessionCode,
        QObject* parent)
        : QObject(parent),
          m_bindAddress(std::move(bindAddress)),
          m_port(port),
          m_tickIntervalMs(tickIntervalMs),
          m_appVersion(std::move(appVersion)),
          m_numDecks(numDecks),
          m_visibleDecks(numDecks),
          m_pQueryHandler(pQueryHandler),
          m_pTcpServer(nullptr),
          m_pWsServer(nullptr),
          m_pPublisher(nullptr) {
    m_pairing.loadTokens(pairedTokensJson);
    m_pairing.setSessionCode(sessionCode);
}

void CompanionServer::setSessionCode(const QString& code) {
    m_pairing.setSessionCode(code);
    kLogger.info() << "Companion pairing code regenerated:" << code;
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
    connect(m_pPublisher,
            &DeckStatePublisher::decksConfigChanged,
            this,
            &CompanionServer::onDecksConfigChanged);
    connect(m_pPublisher,
            &DeckStatePublisher::masterTickReady,
            this,
            [this](const QJsonObject& partialTick) {
                QJsonObject event = partialTick;
                event.insert(QStringLiteral("serverTimeMs"), serverTimeMs());
                broadcast(event, /*droppable*/ true);
            });
    m_pPublisher->setDecks(m_numDecks);
    m_pPublisher->start();

    // The session pairing code (generated on the main thread and passed in) is
    // valid for the whole session with full control scope — what the user types
    // on the phone. Log it prominently so it is obvious.
    kLogger.info() << "=====================================";
    kLogger.info() << "Companion pairing code:" << m_pairing.sessionCode();
    kLogger.info() << "=====================================";

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
    m_clientTopics.clear();
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
        if (m_activeHttpConnections >= kMaxHttpConnections) {
            pSocket->abort();
            pSocket->deleteLater();
            continue;
        }
        m_activeHttpConnections++;
        auto* pConnection = new HttpConnection(pSocket, kMaxRequestBytes, this);
        connect(pConnection, &QObject::destroyed, this, [this]() {
            m_activeHttpConnections--;
        });
        pConnection->setRouter(
                [this](const HttpRequest& request) { return route(request); });
        connect(pConnection,
                &HttpConnection::webSocketUpgradeRequested,
                this,
                &CompanionServer::onWebSocketUpgradeRequested);
    }
}

void CompanionServer::onWebSocketUpgradeRequested(QTcpSocket* pSocket,
        const QByteArray& token,
        bool fromLoopback,
        const QString& peerAddress) {
    const qint64 nowMs = serverTimeMs();
    const bool throttled =
            !fromLoopback && isAuthThrottled(peerAddress, nowMs);
    // WS clients only receive events (read-only), so control scope is not needed.
    if (throttled ||
            m_pairing.authorize(fromLoopback, token, /*needsControl*/ false) !=
                    PairingManager::AuthResult::Ok) {
        if (!throttled && !fromLoopback) {
            recordAuthFailure(peerAddress, nowMs);
        }
        pSocket->write(throttled
                        ? "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n"
                          "Connection: close\r\n\r\n"
                        : "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n"
                          "Connection: close\r\n\r\n");
        pSocket->flush();
        pSocket->disconnectFromHost();
        pSocket->deleteLater();
        return;
    }
    if (!fromLoopback) {
        clearAuthFailures(peerAddress);
    }
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
        m_clientTopics.insert(pClient, {QStringLiteral("decks")});
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
    m_clientTopics.remove(pClient);
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
        return;
    }
    if (type == QLatin1String("hello")) {
        const int protocol = obj.value(QStringLiteral("protocol")).toInt(1);
        if (protocol != 1) {
            kLogger.warning()
                    << "Companion client"
                    << obj.value(QStringLiteral("clientName")).toString()
                    << "speaks unsupported protocol" << protocol;
        }
        return;
    }
    if (type == QLatin1String("subscribe")) {
        // Topics: "decks" (deck.*, decks.config, master.tick) and "library"
        // (library.*). Default is decks-only per the spec.
        QSet<QString> topics;
        const QJsonArray requested =
                obj.value(QStringLiteral("topics")).toArray();
        for (const QJsonValue& value : requested) {
            const QString topic = value.toString();
            if (topic == QLatin1String("decks") ||
                    topic == QLatin1String("library")) {
                topics.insert(topic);
            }
        }
        if (topics.isEmpty()) {
            topics.insert(QStringLiteral("decks"));
        }
        const bool addedLibrary =
                topics.contains(QStringLiteral("library")) &&
                !m_clientTopics.value(pClient).contains(
                        QStringLiteral("library"));
        m_clientTopics.insert(pClient, topics);
        if (addedLibrary) {
            // Late subscription: catch the client up on the current view.
            sendLibraryReplay(pClient);
        }
        return;
    }
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
    broadcast(event, /*droppable*/ true);
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
    broadcast(event, /*droppable*/ false);
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
    broadcast(event, /*droppable*/ false);
}

void CompanionServer::onNumberOfDecksChanged(int numDecks) {
    m_numDecks = numDecks;
    if (m_pPublisher) {
        m_pPublisher->setDecks(numDecks);
    }
}

void CompanionServer::onDecksConfigChanged(int numDecks, int visibleDecks) {
    m_numDecks = numDecks;
    m_visibleDecks = visibleDecks;
    if (m_pPublisher) {
        m_pPublisher->setDecks(numDecks);
    }
    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("decks.config"));
    event.insert(QStringLiteral("numDecks"), numDecks);
    event.insert(QStringLiteral("visibleDecks"), visibleDecks);
    event.insert(QStringLiteral("serverTimeMs"), serverTimeMs());
    broadcast(event, /*droppable*/ false);
}

void CompanionServer::onLibraryView(const QJsonObject& event) {
    m_lastLibraryView = event;
    m_lastLibraryCursor = QJsonObject(); // cursor is stale in a new view
    broadcast(event, /*droppable*/ false);
}

void CompanionServer::onLibraryCursor(const QJsonObject& event) {
    m_lastLibraryCursor = event;
    broadcast(event, /*droppable*/ true);
}

void CompanionServer::onLibraryChanged(const QJsonObject& event) {
    broadcast(event, /*droppable*/ false);
}

void CompanionServer::sendReplay(QWebSocket* pClient) {
    // Deck configuration first so the client lays out 2 vs 4 decks correctly.
    {
        QJsonObject config;
        config.insert(QStringLiteral("type"), QStringLiteral("decks.config"));
        config.insert(QStringLiteral("numDecks"), m_numDecks);
        config.insert(QStringLiteral("visibleDecks"), m_visibleDecks);
        config.insert(QStringLiteral("serverTimeMs"), serverTimeMs());
        pClient->sendTextMessage(QString::fromUtf8(
                QJsonDocument(config).toJson(QJsonDocument::Compact)));
    }
    // Library replay is sent when a client subscribes to the "library" topic
    // (default subscription is decks-only per the spec).
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

void CompanionServer::sendLibraryReplay(QWebSocket* pClient) {
    if (!m_lastLibraryView.isEmpty()) {
        pClient->sendTextMessage(QString::fromUtf8(
                QJsonDocument(m_lastLibraryView).toJson(QJsonDocument::Compact)));
    }
    if (!m_lastLibraryCursor.isEmpty()) {
        pClient->sendTextMessage(QString::fromUtf8(QJsonDocument(
                m_lastLibraryCursor).toJson(QJsonDocument::Compact)));
    }
}

void CompanionServer::broadcast(const QJsonObject& event, bool droppable) {
    if (m_clients.isEmpty()) {
        return;
    }
    const QString payload = QString::fromUtf8(
            QJsonDocument(event).toJson(QJsonDocument::Compact));
    const QString topic =
            event.value(QStringLiteral("type")).toString().startsWith(
                    QLatin1String("library."))
            ? QStringLiteral("library")
            : QStringLiteral("decks");
    // Iterate over a copy: disconnecting a client mutates m_clients.
    const QList<QWebSocket*> clients = m_clients;
    for (QWebSocket* pClient : clients) {
        if (!m_clientTopics.value(pClient).contains(topic)) {
            continue;
        }
        const qint64 buffered = pClient->bytesToWrite();
        if (buffered > kWsHardBufferCap) {
            // The peer stopped draining; cut it loose before we balloon.
            kLogger.warning()
                    << "Companion client too slow (buffered" << buffered
                    << "bytes); disconnecting";
            pClient->close(QWebSocketProtocol::CloseCodeGoingAway);
            continue;
        }
        if (droppable && buffered > kWsSoftBufferCap) {
            // Ticks/cursor updates are refreshed continuously; skipping one for
            // a congested client is invisible, buffering all of them is not.
            continue;
        }
        pClient->sendTextMessage(payload);
    }
}

qint64 CompanionServer::serverTimeMs() const {
    return m_uptime.isValid() ? m_uptime.elapsed() : 0;
}

bool CompanionServer::isAuthThrottled(const QString& peer, qint64 nowMs) {
    auto it = m_authFailures.find(peer);
    if (it == m_authFailures.end()) {
        return false;
    }
    if (nowMs - it->lastFailureMs > kAuthFailureExpiryMs) {
        m_authFailures.erase(it);
        return false;
    }
    return nowMs < it->lockedUntilMs;
}

void CompanionServer::recordAuthFailure(const QString& peer, qint64 nowMs) {
    // Bound the tracker so an address-rotating attacker cannot balloon memory.
    if (m_authFailures.size() >= kMaxTrackedPeers &&
            !m_authFailures.contains(peer)) {
        m_authFailures.clear();
    }
    AuthFailures& record = m_authFailures[peer];
    record.count++;
    record.lastFailureMs = nowMs;
    if (record.count >= kAuthFailuresBeforeLockout) {
        // Exponential lockout: 30s, 60s, 120s, ... capped at ~30 min.
        const int excess =
                qMin(record.count - kAuthFailuresBeforeLockout, 6);
        record.lockedUntilMs = nowMs + (kAuthLockoutBaseMs << excess);
        kLogger.warning() << "Companion auth lockout for" << peer << "after"
                          << record.count << "failures";
    }
}

void CompanionServer::clearAuthFailures(const QString& peer) {
    m_authFailures.remove(peer);
}

HttpResponse CompanionServer::route(const HttpRequest& request) {
    const QStringList segments = request.path.split('/', Qt::SkipEmptyParts);

    // Brute-force guard runs before anything else for non-loopback peers.
    const qint64 nowMs = serverTimeMs();
    if (!request.fromLoopback && isAuthThrottled(request.peerAddress, nowMs)) {
        return HttpResponse::error(
                429, "too_many_attempts", "auth throttled; retry later");
    }

    // Pairing endpoints carry their own auth rules (loopback / code-gated).
    if (segments.size() >= 2 && segments.at(0) == QLatin1String("v1") &&
            segments.at(1) == QLatin1String("pair")) {
        // Failed claims also count toward the per-IP throttle.
        HttpResponse response = handlePairing(segments, request);
        if (!request.fromLoopback) {
            if (response.status == 401) {
                recordAuthFailure(request.peerAddress, nowMs);
            } else if (response.status == 200) {
                clearAuthFailures(request.peerAddress);
            }
        }
        return response;
    }

    // Auth gate for everything else: loopback is trusted; LAN needs a valid
    // token, and any write (POST) needs a control-scope token.
    const bool needsControl = (request.method == "POST");
    switch (m_pairing.authorize(
            request.fromLoopback, request.bearerToken(), needsControl)) {
    case PairingManager::AuthResult::Ok:
        if (!request.fromLoopback) {
            clearAuthFailures(request.peerAddress);
        }
        break;
    case PairingManager::AuthResult::Unauthorized:
        if (!request.fromLoopback) {
            recordAuthFailure(request.peerAddress, nowMs);
        }
        return HttpResponse::error(401, "unauthorized");
    case PairingManager::AuthResult::Forbidden:
        return HttpResponse::error(
                403, "action_not_allowed", "read-only token");
    }

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
        if (segments.size() == 4 &&
                segments.at(3) == QLatin1String("beatgrid")) {
            QByteArray json;
            const bool ok = QMetaObject::invokeMethod(m_pQueryHandler,
                    "getTrackBeatgrid",
                    Qt::BlockingQueuedConnection,
                    Q_RETURN_ARG(QByteArray, json),
                    Q_ARG(int, trackId));
            if (!ok) {
                return HttpResponse::error(500, "internal");
            }
            if (json.isEmpty()) {
                return HttpResponse::error(404, "not_found", "no such track");
            }
            return HttpResponse::json(200, json);
        }
        if (segments.size() == 4 && segments.at(3) == QLatin1String("cover")) {
            QByteArray jpeg;
            const bool ok = QMetaObject::invokeMethod(m_pQueryHandler,
                    "getTrackCover",
                    Qt::BlockingQueuedConnection,
                    Q_RETURN_ARG(QByteArray, jpeg),
                    Q_ARG(int, trackId));
            if (!ok) {
                return HttpResponse::error(500, "internal", "cover load failed");
            }
            if (jpeg.isEmpty()) {
                return HttpResponse::error(404, "not_found", "no cover art");
            }
            HttpResponse response;
            response.status = 200;
            response.contentType = "image/jpeg";
            response.body = jpeg;
            return response;
        }
        if (segments.size() == 5 &&
                segments.at(3) == QLatin1String("waveform") &&
                segments.at(4) == QLatin1String("summary")) {
            return handleWaveformSummary(trackId);
        }
        return HttpResponse::error(404, "not_found");
    }

    // Library container listings (playlists / crates / history) — all served
    // by main-thread SQL via the query handler.
    const auto blockingJson = [this](const char* member,
                                      int arg,
                                      bool hasArg) -> QByteArray {
        QByteArray json;
        bool ok = false;
        if (hasArg) {
            ok = QMetaObject::invokeMethod(m_pQueryHandler,
                    member,
                    Qt::BlockingQueuedConnection,
                    Q_RETURN_ARG(QByteArray, json),
                    Q_ARG(int, arg));
        } else {
            ok = QMetaObject::invokeMethod(m_pQueryHandler,
                    member,
                    Qt::BlockingQueuedConnection,
                    Q_RETURN_ARG(QByteArray, json));
        }
        return ok ? json : QByteArray();
    };
    if (request.method == "GET" && segments.size() >= 2 &&
            segments.at(0) == QLatin1String("v1") &&
            (segments.at(1) == QLatin1String("playlists") ||
                    segments.at(1) == QLatin1String("crates"))) {
        const bool isPlaylist = segments.at(1) == QLatin1String("playlists");
        if (segments.size() == 2) {
            const QByteArray json = blockingJson(
                    isPlaylist ? "getPlaylists" : "getCrates", 0, false);
            if (json.isEmpty()) {
                return HttpResponse::error(500, "internal");
            }
            return HttpResponse::json(200, json);
        }
        if (segments.size() == 4 && segments.at(3) == QLatin1String("tracks")) {
            bool idOk = false;
            const int id = segments.at(2).toInt(&idOk);
            if (!idOk) {
                return HttpResponse::error(400, "bad_request", "invalid id");
            }
            const QByteArray json = blockingJson(
                    isPlaylist ? "getPlaylistTracks" : "getCrateTracks",
                    id,
                    true);
            if (json.isEmpty()) {
                return HttpResponse::error(404, "not_found");
            }
            return HttpResponse::json(200, json);
        }
        return HttpResponse::error(404, "not_found");
    }
    if (request.method == "GET" &&
            request.path == QLatin1String("/v1/history/current/tracks")) {
        const QByteArray json = blockingJson("getHistoryTracks", 0, false);
        if (json.isEmpty()) {
            return HttpResponse::error(500, "internal");
        }
        return HttpResponse::json(200, json);
    }

    // POST /v1/library/{move,scroll,goto,focus} — remote library navigation
    if (request.method == "POST" && segments.size() == 3 &&
            segments.at(0) == QLatin1String("v1") &&
            segments.at(1) == QLatin1String("library")) {
        return handleLibraryNav(segments.at(2), request);
    }

    // POST /v1/autodj/queue
    if (request.method == "POST" &&
            request.path == QLatin1String("/v1/autodj/queue")) {
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        if (!body.contains(QStringLiteral("trackId"))) {
            return HttpResponse::error(400, "bad_request", "trackId required");
        }
        emit autoDjQueueRequested(body.value(QStringLiteral("trackId")).toInt());
        return HttpResponse::json(202, "{\"ok\":true,\"status\":\"queued\"}");
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
    // Always emits the full DeckStateDto shape (all required fields present)
    // so strictly-typed clients never fail on empty or freshly-loaded decks.
    QJsonObject dto;
    dto.insert(QStringLiteral("deck"), deck);

    const auto it = m_deckSnapshots.constFind(deck);
    const bool haveSnapshot = it != m_deckSnapshots.constEnd();
    dto.insert(QStringLiteral("generation"),
            haveSnapshot ? static_cast<qint64>(it->generation) : 0);

    QJsonObject tick;
    double trackDuration = 0.0;
    if (haveSnapshot) {
        tick = it->lastTick;
        if (it->loaded && it->loadedEvent.contains(QStringLiteral("track"))) {
            const QJsonObject track =
                    it->loadedEvent.value(QStringLiteral("track")).toObject();
            dto.insert(QStringLiteral("track"), track);
            // Until the first tick after a load arrives, fall back to the
            // track's own duration instead of reporting 0.
            trackDuration =
                    track.value(QStringLiteral("durationSeconds")).toDouble(0.0);
        }
    }
    dto.insert(QStringLiteral("playposition"),
            tick.value(QStringLiteral("playposition")).toDouble(0.0));
    dto.insert(QStringLiteral("positionSeconds"),
            tick.value(QStringLiteral("positionSeconds")).toDouble(0.0));
    dto.insert(QStringLiteral("durationSeconds"),
            tick.value(QStringLiteral("durationSeconds")).toDouble(trackDuration));
    dto.insert(QStringLiteral("rate"),
            tick.value(QStringLiteral("rate")).toDouble(1.0));
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

HttpResponse CompanionServer::handleLibraryNav(
        const QString& action, const HttpRequest& request) {
    // Drives the same [Library] controls hardware controllers use, so the
    // phone mirrors exactly what a browse knob does. ControlObject::set is
    // thread-safe; no main-thread hop needed. The resulting view/cursor
    // changes flow back as library.view / library.cursor events.
    const QJsonObject body = QJsonDocument::fromJson(request.body).object();
    const int delta = body.value(QStringLiteral("delta")).toInt(1);
    const QString group = QStringLiteral("[Library]");

    if (action == QLatin1String("move")) {
        // Move the cursor by delta rows (encoder semantics; negative = up).
        ControlObject::set(
                ConfigKey(group, QStringLiteral("MoveVertical")), delta);
        return HttpResponse::json(200, "{\"ok\":true}");
    }
    if (action == QLatin1String("scroll")) {
        // Page-wise scrolling (pageup/pagedown semantics).
        ControlObject::set(
                ConfigKey(group, QStringLiteral("ScrollVertical")), delta);
        return HttpResponse::json(200, "{\"ok\":true}");
    }
    if (action == QLatin1String("focus")) {
        // Move keyboard focus between library panes (sidebar <-> track list).
        ControlObject::set(
                ConfigKey(group, QStringLiteral("MoveFocus")), delta);
        return HttpResponse::json(200, "{\"ok\":true}");
    }
    if (action == QLatin1String("goto")) {
        // Activate the highlighted item (expand/enter sidebar item, or the
        // configured GoToItem behavior in the track list).
        ControlObject::set(ConfigKey(group, QStringLiteral("GoToItem")), 1.0);
        ControlObject::set(ConfigKey(group, QStringLiteral("GoToItem")), 0.0);
        return HttpResponse::json(200, "{\"ok\":true}");
    }
    return HttpResponse::error(403, "action_not_allowed", "unknown action");
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
        // Push-button semantics: press + release, otherwise the control stays
        // at 1.0 and subsequent syncs are edge-less no-ops.
        ControlObject::set(ConfigKey(group, QStringLiteral("beatsync")), 1.0);
        ControlObject::set(ConfigKey(group, QStringLiteral("beatsync")), 0.0);
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
    if (action == "loadSelected") {
        // Load the track currently highlighted in the library (the HUD cursor)
        // to this deck — same control a hardware "load" button uses.
        const QJsonObject body =
                QJsonDocument::fromJson(request.body).object();
        const bool play = body.value(QStringLiteral("play")).toBool();
        const QString control = play
                ? QStringLiteral("LoadSelectedTrackAndPlay")
                : QStringLiteral("LoadSelectedTrack");
        ControlObject::set(ConfigKey(group, control), 1.0);
        ControlObject::set(ConfigKey(group, control), 0.0);
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

HttpResponse CompanionServer::handlePairing(
        const QStringList& segments, const HttpRequest& request) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    // POST /v1/pair  (loopback only) -> open a pairing window
    if (segments.size() == 2 && request.method == "POST") {
        if (!request.fromLoopback) {
            return HttpResponse::error(
                    403, "action_not_allowed", "pairing must be started locally");
        }
        constexpr int kTtlSeconds = 60;
        const QString code = m_pairing.beginPairing(kTtlSeconds, nowMs);
        QJsonObject obj;
        obj.insert(QStringLiteral("code"), code);
        obj.insert(QStringLiteral("expiresInSeconds"), kTtlSeconds);
        obj.insert(QStringLiteral("qr"),
                QStringLiteral("mixxx-companion://pair?port=%1&code=%2")
                        .arg(m_port)
                        .arg(code));
        kLogger.info() << "Companion pairing window opened for" << kTtlSeconds
                       << "s";
        return HttpResponse::json(
                200, QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    // POST /v1/pair/claim  (code-gated, any origin)
    if (segments.size() == 3 && segments.at(2) == QLatin1String("claim") &&
            request.method == "POST") {
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        const QString code = body.value(QStringLiteral("code")).toString();
        const QString deviceName = body.value(QStringLiteral("deviceName"))
                                           .toString(QStringLiteral("unknown"));
        const bool readOnly =
                body.value(QStringLiteral("readOnly")).toBool(false);
        if (code.isEmpty()) {
            return HttpResponse::error(400, "bad_request", "code required");
        }
        const PairingManager::ClaimResult result =
                m_pairing.claim(code, deviceName, readOnly, nowMs);
        if (!result.ok) {
            const int status =
                    result.error == QLatin1String("too_many_attempts") ? 429 : 401;
            return HttpResponse::error(status, result.error.toUtf8());
        }
        emit persistTokens(m_pairing.serializeTokens());
        QJsonObject obj;
        obj.insert(QStringLiteral("token"), result.token);
        obj.insert(QStringLiteral("readOnly"), readOnly);
        return HttpResponse::json(
                200, QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    // GET /v1/pair/code  (loopback only) — the session pairing code, so a
    // local helper/overlay can display it to the user.
    if (segments.size() == 3 && segments.at(2) == QLatin1String("code") &&
            request.method == "GET") {
        if (!request.fromLoopback) {
            return HttpResponse::error(403, "action_not_allowed", "local only");
        }
        QJsonObject obj;
        obj.insert(QStringLiteral("code"), m_pairing.sessionCode());
        obj.insert(QStringLiteral("port"), m_port);
        return HttpResponse::json(
                200, QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    // GET /v1/pair/devices  (loopback only)
    if (segments.size() == 3 && segments.at(2) == QLatin1String("devices") &&
            request.method == "GET") {
        if (!request.fromLoopback) {
            return HttpResponse::error(403, "action_not_allowed", "local only");
        }
        return HttpResponse::json(200, m_pairing.serializeDevices().toUtf8());
    }

    // DELETE /v1/pair/devices/:id  (loopback only)
    if (segments.size() == 4 && segments.at(2) == QLatin1String("devices") &&
            request.method == "DELETE") {
        if (!request.fromLoopback) {
            return HttpResponse::error(403, "action_not_allowed", "local only");
        }
        if (!m_pairing.revokeDevice(segments.at(3).toUtf8())) {
            return HttpResponse::error(404, "not_found", "no such device");
        }
        emit persistTokens(m_pairing.serializeTokens());
        return HttpResponse::json(200, "{\"ok\":true}");
    }

    return HttpResponse::error(404, "not_found");
}

HttpResponse CompanionServer::handleStatus() {
    QJsonObject status;
    status.insert(QStringLiteral("app"), QStringLiteral("Mixxx"));
    status.insert(QStringLiteral("version"), m_appVersion);
    status.insert(QStringLiteral("apiVersion"), kApiVersion);
    status.insert(QStringLiteral("numDecks"), m_numDecks);
    status.insert(QStringLiteral("visibleDecks"), m_visibleDecks);
    status.insert(QStringLiteral("libraryReady"), true);
    status.insert(QStringLiteral("uptimeMs"), serverTimeMs());
    status.insert(QStringLiteral("clients"), m_clients.size());
    // Loopback-only bind is open; LAN-exposed binds require the session
    // pairing code (or a long-lived token).
    status.insert(QStringLiteral("auth"),
            m_bindAddress == QHostAddress(QHostAddress::LocalHost)
                    ? QStringLiteral("open-loopback")
                    : QStringLiteral("code"));
    status.insert(QStringLiteral("pairedDevices"), m_pairing.hasPairedDevices());
    return HttpResponse::json(
            200, QJsonDocument(status).toJson(QJsonDocument::Compact));
}

} // namespace companion
} // namespace mixxx

#include "moc_companionserver.cpp"
