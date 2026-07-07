#pragma once

#include <QHostAddress>
#include <QString>

#include "companion/companiondefs.h"
#include "preferences/usersettings.h"

namespace mixxx {
namespace companion {

/// Typed, read-only view over the `[CompanionAPI]` configuration group.
///
/// This is a thin value wrapper around the shared `UserSettingsPointer`; it owns
/// no state of its own and is cheap to copy. All keys have safe defaults so the
/// service is fully off unless explicitly enabled.
class CompanionSettings {
  public:
    explicit CompanionSettings(UserSettingsPointer pConfig)
            : m_pConfig(std::move(pConfig)) {
    }

    /// Master switch. When false the service does not start and has zero cost.
    bool isEnabled() const {
        return m_pConfig->getValue<bool>(
                ConfigKey(kGroup, QStringLiteral("enabled")), false);
    }

    /// TCP port shared by the HTTP and WebSocket endpoints.
    quint16 port() const {
        int value = m_pConfig->getValue<int>(
                ConfigKey(kGroup, QStringLiteral("port")), kDefaultPort);
        if (value <= 0 || value > 65535) {
            return kDefaultPort;
        }
        return static_cast<quint16>(value);
    }

    /// When true the listener binds all interfaces (LAN reachable); otherwise it
    /// binds loopback only. LAN exposure additionally requires pairing (T08).
    bool allowLan() const {
        return m_pConfig->getValue<bool>(
                ConfigKey(kGroup, QStringLiteral("allow_lan")), false);
    }

    /// Address the listener binds to, derived from allowLan().
    QHostAddress bindAddress() const {
        return allowLan() ? QHostAddress(QHostAddress::Any)
                          : QHostAddress(QHostAddress::LocalHost);
    }

    /// Deck tick cadence in milliseconds, clamped to the supported range.
    int tickIntervalMs() const {
        int value = m_pConfig->getValue<int>(
                ConfigKey(kGroup, QStringLiteral("tick_interval_ms")),
                kDefaultTickIntervalMs);
        return qBound(kMinTickIntervalMs, value, kMaxTickIntervalMs);
    }

    /// When true, `TrackDto.location` (the absolute file path) may be exposed.
    /// Off by default: paths are treated as privileged and never leave the
    /// machine unless the operator opts in.
    bool exposeFilePaths() const {
        return m_pConfig->getValue<bool>(
                ConfigKey(kGroup, QStringLiteral("expose_file_paths")), false);
    }

    /// Opaque JSON blob of paired device tokens (hashed). Persisted so pairings
    /// survive restarts.
    QString pairedTokens() const {
        return m_pConfig->getValue(
                ConfigKey(kGroup, QStringLiteral("paired_tokens")));
    }
    void setPairedTokens(const QString& json) const {
        m_pConfig->set(ConfigKey(kGroup, QStringLiteral("paired_tokens")),
                ConfigValue(json));
    }

  private:
    static constexpr char kGroup[] = "[CompanionAPI]";

    UserSettingsPointer m_pConfig;
};

} // namespace companion
} // namespace mixxx
