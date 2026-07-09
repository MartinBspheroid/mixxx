#include "companion/httpconnection.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("Companion");

// Guard rail so a hostile or buggy client cannot make us buffer unbounded data
// before a full request line + headers arrive.
constexpr int kMaxHeaderBytes = 16 * 1024;

// A connection that has not produced a complete request within this window is
// aborted (slowloris / FD-exhaustion guard).
constexpr int kIdleTimeoutMs = 15 * 1000;

QByteArray reasonPhrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
    default:
        return "Internal Server Error";
    }
}
} // namespace

namespace mixxx {
namespace companion {

HttpResponse HttpResponse::error(int status,
        const QByteArray& code,
        const QByteArray& message) {
    QJsonObject err;
    err.insert(QStringLiteral("code"), QString::fromUtf8(code));
    if (!message.isEmpty()) {
        err.insert(QStringLiteral("message"), QString::fromUtf8(message));
    }
    QJsonObject root;
    root.insert(QStringLiteral("error"), err);
    HttpResponse r;
    r.status = status;
    r.body = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return r;
}

HttpConnection::HttpConnection(
        QTcpSocket* pSocket, qint64 maxRequestBytes, QObject* parent)
        : QObject(parent),
          m_pSocket(pSocket),
          m_maxRequestBytes(maxRequestBytes),
          m_classified(false),
          m_responded(false) {
    m_pSocket->setParent(this);
    connect(m_pSocket, &QTcpSocket::readyRead, this, &HttpConnection::onReadyRead);
    connect(m_pSocket,
            &QTcpSocket::disconnected,
            this,
            &HttpConnection::onDisconnected);
    // Idle guard: a peer that never completes its request (slowloris) must not
    // hold this connection open indefinitely.
    QTimer::singleShot(kIdleTimeoutMs, this, [this]() {
        if (!m_responded && m_pSocket) {
            m_pSocket->abort();
            deleteLater();
        }
    });
    // A client may have sent the whole request before we were constructed. Do
    // not process it synchronously here: the caller connects our
    // webSocketUpgradeRequested signal only after construction, so processing
    // must be deferred to the event loop to avoid emitting into the void.
    if (m_pSocket->bytesAvailable() > 0) {
        QMetaObject::invokeMethod(this, "onReadyRead", Qt::QueuedConnection);
    }
}

HttpConnection::~HttpConnection() = default;

void HttpConnection::onReadyRead() {
    if (!m_classified) {
        classifyAndDispatch();
        return;
    }
    handleHttpRequest();
}

void HttpConnection::classifyAndDispatch() {
    // Peek (do not consume): if this turns out to be a WebSocket handshake we
    // must hand the socket to QWebSocketServer with its buffer intact.
    const QByteArray peeked = m_pSocket->peek(kMaxHeaderBytes + 1);
    const int headerEnd = peeked.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (peeked.size() > kMaxHeaderBytes) {
            fail(431, "bad_request");
        }
        return; // wait for more bytes
    }

    const QByteArray headerBlock = peeked.left(headerEnd).toLower();
    const bool isGet = peeked.startsWith("GET ");
    const bool hasUpgrade = headerBlock.contains("upgrade: websocket") ||
            headerBlock.contains("upgrade:websocket");
    const bool connUpgrade = headerBlock.contains("connection:") &&
            headerBlock.contains("upgrade");
    const bool hasKey = headerBlock.contains("sec-websocket-key:");

    if (isGet && hasUpgrade && connUpgrade && hasKey) {
        // Extract the auth token (?token=... on the WS URL) and loopback flag so
        // the server can enforce auth before completing the upgrade.
        HttpRequest wsRequest;
        parseRequest(peeked.left(headerEnd + 4), &wsRequest);
        const QByteArray token = wsRequest.bearerToken();
        const bool fromLoopback =
                m_pSocket->peerAddress().isLoopback();
        const QString peer = m_pSocket->peerAddress().toString();

        // Hand off to the WebSocket server. Detach so our deleteLater() does not
        // take the socket down with us.
        QTcpSocket* pSocket = m_pSocket;
        disconnect(pSocket, nullptr, this, nullptr);
        pSocket->setParent(nullptr);
        m_pSocket = nullptr;
        m_responded = true; // cancel the idle-timeout abort
        emit webSocketUpgradeRequested(pSocket, token, fromLoopback, peer);
        deleteLater();
        return;
    }

    // Plain HTTP from here on: switch to consuming reads.
    m_classified = true;
    handleHttpRequest();
}

void HttpConnection::handleHttpRequest() {
    if (m_responded) {
        return;
    }
    // In HTTP mode we accumulate into the socket's own read buffer and inspect
    // it via peek() so the offsets stay stable until the full request is present.
    const QByteArray raw = m_pSocket->peek(m_maxRequestBytes + 1);
    if (raw.size() > m_maxRequestBytes) {
        fail(413, "bad_request");
        return;
    }
    const int headerEnd = raw.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return; // headers not complete yet
    }

    // Determine the declared body length, if any.
    int contentLength = 0;
    {
        const QByteArray headerBlock = raw.left(headerEnd);
        for (const QByteArray& line : headerBlock.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            const int colon = trimmed.indexOf(':');
            if (colon <= 0) {
                continue;
            }
            if (trimmed.left(colon).toLower() == "content-length") {
                contentLength = trimmed.mid(colon + 1).trimmed().toInt();
                break;
            }
        }
    }
    const int total = headerEnd + 4 + qMax(0, contentLength);
    if (raw.size() < total) {
        return; // body not fully received yet
    }

    HttpRequest request;
    if (!parseRequest(raw.left(total), &request)) {
        fail(400, "bad_request");
        return;
    }
    request.fromLoopback = m_pSocket && m_pSocket->peerAddress().isLoopback();
    request.peerAddress =
            m_pSocket ? m_pSocket->peerAddress().toString() : QString();

    // CORS preflight: answer directly so browser dashboard clients work.
    if (request.method == "OPTIONS") {
        HttpResponse preflight;
        preflight.status = 204;
        preflight.body.clear();
        writeResponse(preflight);
        return;
    }

    HttpResponse response;
    if (m_router) {
        response = m_router(request);
    } else {
        response = HttpResponse::error(500, "internal", "no router configured");
    }
    writeResponse(response);
}

bool HttpConnection::parseRequest(const QByteArray& raw, HttpRequest* pOut) {
    const int headerEnd = raw.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return false;
    }
    const QByteArray headerBlock = raw.left(headerEnd);
    const QList<QByteArray> lines = headerBlock.split('\n');
    if (lines.isEmpty()) {
        return false;
    }

    // Request line: METHOD SP target SP HTTP/x.y
    const QByteArray requestLine = lines.first().trimmed();
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 3) {
        return false;
    }
    pOut->method = parts.at(0).toUpper();

    const QByteArray target = parts.at(1);
    const int qmark = target.indexOf('?');
    QByteArray pathPart = qmark < 0 ? target : target.left(qmark);
    const QByteArray queryPart = qmark < 0 ? QByteArray() : target.mid(qmark + 1);
    pOut->path = QUrl::fromPercentEncoding(pathPart);
    pOut->query = QUrlQuery(QString::fromUtf8(queryPart));

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        pOut->headers.insert(name, value);
    }

    pOut->body = raw.mid(headerEnd + 4);
    return true;
}

void HttpConnection::writeResponse(const HttpResponse& response) {
    if (m_responded || !m_pSocket) {
        return;
    }
    m_responded = true;

    QByteArray out;
    out.reserve(response.body.size() + 128);
    out += "HTTP/1.1 ";
    out += QByteArray::number(response.status);
    out += ' ';
    out += reasonPhrase(response.status);
    out += "\r\n";
    out += "Content-Type: " + response.contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
    // CORS: allow browser dashboard clients from any origin. Bearer/code auth
    // still applies; these headers only unblock the browser's fetch layer.
    out += "Access-Control-Allow-Origin: *\r\n";
    out += "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
    out += "Access-Control-Allow-Headers: Authorization, Content-Type\r\n";
    out += "Access-Control-Max-Age: 86400\r\n";
    // v1 is one-request-per-connection; keeps the parser and lifetime trivial.
    out += "Connection: close\r\n";
    out += "\r\n";
    out += response.body;

    m_pSocket->write(out);
    m_pSocket->flush();
    m_pSocket->disconnectFromHost();
}

void HttpConnection::fail(int status, const QByteArray& code) {
    writeResponse(HttpResponse::error(status, code));
}

void HttpConnection::onDisconnected() {
    deleteLater();
}

} // namespace companion
} // namespace mixxx

#include "moc_httpconnection.cpp"
