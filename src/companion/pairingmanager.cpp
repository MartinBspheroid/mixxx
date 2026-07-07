#include "companion/pairingmanager.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

namespace {
constexpr int kMaxClaimAttempts = 5;
} // namespace

namespace mixxx {
namespace companion {

QByteArray PairingManager::hashToken(const QByteArray& token) {
    return QCryptographicHash::hash(token, QCryptographicHash::Sha256).toHex();
}

QString PairingManager::generateCode() {
    // 6-digit numeric code (000000-999999).
    const quint32 value = QRandomGenerator::system()->bounded(1000000u);
    return QStringLiteral("%1").arg(value, 6, 10, QLatin1Char('0'));
}

QByteArray PairingManager::generateToken() {
    // 32 random bytes, hex-encoded.
    QByteArray raw(32, '\0');
    QRandomGenerator::system()->fillRange(
            reinterpret_cast<quint32*>(raw.data()), 32 / sizeof(quint32));
    return raw.toHex();
}

void PairingManager::loadTokens(const QString& json) {
    m_tokens.clear();
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray()) {
        return;
    }
    const QJsonArray array = doc.array();
    for (const QJsonValue& value : array) {
        const QJsonObject obj = value.toObject();
        const QByteArray id =
                obj.value(QStringLiteral("id")).toString().toLatin1();
        if (id.isEmpty()) {
            continue;
        }
        TokenRecord record;
        record.deviceName = obj.value(QStringLiteral("deviceName")).toString();
        record.createdAtIso =
                obj.value(QStringLiteral("createdAtIso")).toString();
        record.readOnly = obj.value(QStringLiteral("readOnly")).toBool();
        m_tokens.insert(id, record);
    }
}

QString PairingManager::serializeTokens() const {
    QJsonArray array;
    for (auto it = m_tokens.constBegin(); it != m_tokens.constEnd(); ++it) {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), QString::fromLatin1(it.key()));
        obj.insert(QStringLiteral("deviceName"), it->deviceName);
        obj.insert(QStringLiteral("createdAtIso"), it->createdAtIso);
        obj.insert(QStringLiteral("readOnly"), it->readOnly);
        array.append(obj);
    }
    return QString::fromUtf8(
            QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString PairingManager::serializeDevices() const {
    // Same shape as serializeTokens(); `id` is the opaque token-hash handle and
    // is safe to expose (it is not the token itself).
    return serializeTokens();
}

QString PairingManager::beginPairing(int ttlSeconds, qint64 nowMs) {
    m_pendingCode = generateCode();
    m_pendingExpiresMs = nowMs + static_cast<qint64>(ttlSeconds) * 1000;
    m_pendingAttempts = 0;
    return m_pendingCode;
}

PairingManager::ClaimResult PairingManager::claim(const QString& code,
        const QString& deviceName,
        bool readOnly,
        qint64 nowMs) {
    ClaimResult result;
    if (m_pendingCode.isEmpty() || nowMs > m_pendingExpiresMs) {
        result.error = QStringLiteral("no_pairing_window");
        return result;
    }
    if (m_pendingAttempts >= kMaxClaimAttempts) {
        result.error = QStringLiteral("too_many_attempts");
        return result;
    }
    m_pendingAttempts++;
    if (code != m_pendingCode) {
        result.error = QStringLiteral("bad_code");
        return result;
    }

    // Success: consume the window and mint a token.
    m_pendingCode.clear();
    m_pendingExpiresMs = 0;
    m_pendingAttempts = 0;

    const QByteArray token = generateToken();
    TokenRecord record;
    record.deviceName = deviceName;
    record.createdAtIso = QString(); // stamped by the caller (main thread) if desired
    record.readOnly = readOnly;
    m_tokens.insert(hashToken(token), record);

    result.ok = true;
    result.token = QString::fromLatin1(token);
    return result;
}

PairingManager::AuthResult PairingManager::authorize(
        bool fromLoopback, const QByteArray& token, bool needsControl) const {
    if (fromLoopback) {
        return AuthResult::Ok;
    }
    if (token.isEmpty()) {
        return AuthResult::Unauthorized;
    }
    const auto it = m_tokens.constFind(hashToken(token));
    if (it == m_tokens.constEnd()) {
        return AuthResult::Unauthorized;
    }
    if (needsControl && it->readOnly) {
        return AuthResult::Forbidden;
    }
    return AuthResult::Ok;
}

bool PairingManager::revokeDevice(const QByteArray& id) {
    return m_tokens.remove(id) > 0;
}

} // namespace companion
} // namespace mixxx
