#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QUrlQuery>
#include <functional>

class QTcpSocket;

namespace mixxx {
namespace companion {

/// A parsed HTTP/1.1 request. Only the small subset the Companion API needs is
/// modelled; header names are lower-cased for case-insensitive lookup.
struct HttpRequest {
    QByteArray method;              ///< "GET", "POST", ...
    QString path;                   ///< decoded path, e.g. "/v1/decks/1"
    QUrlQuery query;                ///< parsed query string
    QHash<QByteArray, QByteArray> headers; ///< lower-cased header name -> value
    QByteArray body;                ///< request body (POST)
    bool fromLoopback = false;      ///< peer is 127.0.0.1/::1 (trusted)

    QByteArray header(const char* name) const {
        return headers.value(QByteArray(name).toLower());
    }

    /// Token from `Authorization: Bearer <token>`, else the `token` query param.
    QByteArray bearerToken() const {
        const QByteArray auth = header("authorization");
        if (auth.startsWith("Bearer ")) {
            return auth.mid(7).trimmed();
        }
        return query.queryItemValue(QStringLiteral("token")).toUtf8();
    }
};

/// An HTTP response to be written back to the client.
struct HttpResponse {
    int status = 200;
    QByteArray contentType = "application/json; charset=utf-8";
    QByteArray body;

    /// Build a JSON response from an already-serialized document body.
    static HttpResponse json(int status, const QByteArray& jsonBody) {
        HttpResponse r;
        r.status = status;
        r.body = jsonBody;
        return r;
    }

    /// Build a standard error body `{"error":{"code":...,"message":...}}`.
    static HttpResponse error(int status,
            const QByteArray& code,
            const QByteArray& message = QByteArray());
};

/// Handles a single client TCP connection: buffers the incoming bytes, decides
/// whether it is a WebSocket upgrade or a plain HTTP request, parses HTTP, calls
/// the router, and writes the response. One instance per connection; it owns its
/// socket and deletes itself (and the socket) when done.
///
/// Lives entirely on the Companion worker thread.
class HttpConnection : public QObject {
    Q_OBJECT
  public:
    using Router = std::function<HttpResponse(const HttpRequest&)>;

    HttpConnection(QTcpSocket* pSocket, qint64 maxRequestBytes, QObject* parent = nullptr);
    ~HttpConnection() override;

    void setRouter(Router router) {
        m_router = std::move(router);
    }

  signals:
    /// Emitted when the incoming request is a WebSocket handshake. The socket is
    /// handed over with its buffer untouched (classification used peek only), so
    /// the receiver can pass it straight to QWebSocketServer::handleConnection().
    /// This HttpConnection relinquishes ownership of the socket before emitting.
    /// `token` is the WS `?token=` param (or Authorization bearer); fromLoopback
    /// marks a trusted peer. The receiver enforces auth before upgrading.
    void webSocketUpgradeRequested(
            QTcpSocket* pSocket, const QByteArray& token, bool fromLoopback);

  private slots:
    void onReadyRead();
    void onDisconnected();

  private:
    void classifyAndDispatch();
    void handleHttpRequest();
    bool parseRequest(const QByteArray& raw, HttpRequest* pOut);
    void writeResponse(const HttpResponse& response);
    void fail(int status, const QByteArray& code);

    QTcpSocket* m_pSocket;
    qint64 m_maxRequestBytes;
    bool m_classified;
    bool m_responded;
    Router m_router;
};

} // namespace companion
} // namespace mixxx
