#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

namespace mixxx {
namespace companion {

/// Token-based pairing and authentication for the Companion API.
///
/// Loopback clients are trusted and never need a token (decision D5). Non-loopback
/// (LAN) clients must present a bearer token obtained by claiming a short-lived
/// pairing code that the operator generates from a trusted (loopback) context.
///
/// Tokens are stored HASHED (SHA-256); the plaintext token is returned exactly
/// once at claim time and never logged. Time and randomness are injected so the
/// core logic is deterministically unit-testable.
///
/// Lives on the Companion worker thread (consulted per request). Persistence is
/// delegated: loadTokens()/serializeTokens() move an opaque JSON string to/from
/// the settings on the main thread.
class PairingManager {
  public:
    struct TokenRecord {
        QString deviceName;
        QString createdAtIso;
        bool readOnly = false;
    };

    enum class AuthResult {
        Ok,           ///< allowed
        Unauthorized, ///< missing/invalid token (-> 401)
        Forbidden,    ///< valid token but insufficient scope (-> 403)
    };

    struct ClaimResult {
        bool ok = false;
        QString token;   ///< plaintext, returned once on success
        QString error;   ///< error code on failure
    };

    PairingManager() = default;

    /// SHA-256 hex digest of a token. Static so tests can check hashing.
    static QByteArray hashToken(const QByteArray& token);

    /// Replace the in-memory token set from a serialized JSON array string
    /// (as produced by serializeTokens()). Invalid input clears the set.
    void loadTokens(const QString& json);
    /// Serialize the token set to a JSON array string for persistence.
    QString serializeTokens() const;

    /// Begin a pairing window: generate a 6-digit code valid for ttlSeconds.
    /// Replaces any existing window. Returns the code (show it to the operator).
    QString beginPairing(int ttlSeconds, qint64 nowMs);

    /// Exchange a pairing code for a new token. Enforces expiry and a per-window
    /// attempt limit. On success stores the token hash and returns the plaintext.
    ClaimResult claim(const QString& code,
            const QString& deviceName,
            bool readOnly,
            qint64 nowMs);

    /// Authorize a request. fromLoopback bypasses everything. Otherwise the token
    /// must be valid; if needsControl, the token must not be read-only.
    AuthResult authorize(
            bool fromLoopback, const QByteArray& token, bool needsControl) const;

    /// True if at least one device is paired.
    bool hasPairedDevices() const {
        return !m_tokens.isEmpty();
    }

    /// Device list for management endpoints: JSON array of {id, deviceName,
    /// createdAtIso, readOnly}. `id` is the token hash hex (opaque handle).
    QString serializeDevices() const;

    /// Revoke a device by its id (token hash hex). Returns true if removed.
    bool revokeDevice(const QByteArray& id);

  private:
    static QString generateCode();
    static QByteArray generateToken();

    QHash<QByteArray, TokenRecord> m_tokens; ///< key = token hash hex

    QString m_pendingCode;
    qint64 m_pendingExpiresMs = 0;
    int m_pendingAttempts = 0;
};

} // namespace companion
} // namespace mixxx
