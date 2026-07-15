#include "companion/networkinfo.h"

#include <QNetworkInterface>
#include <algorithm>

namespace mixxx {
namespace companion {

bool isPrivateIPv4(const QHostAddress& address) {
    bool ok = false;
    const quint32 raw = address.toIPv4Address(&ok);
    if (!ok) {
        return false;
    }
    if ((raw & 0xFF000000u) == 0x0A000000u) { // 10.0.0.0/8
        return true;
    }
    if ((raw & 0xFFF00000u) == 0xAC100000u) { // 172.16.0.0/12
        return true;
    }
    if ((raw & 0xFFFF0000u) == 0xC0A80000u) { // 192.168.0.0/16
        return true;
    }
    return false;
}

bool isSharedAddressSpaceIPv4(const QHostAddress& address) {
    bool ok = false;
    const quint32 raw = address.toIPv4Address(&ok);
    if (!ok) {
        return false;
    }
    return (raw & 0xFFC00000u) == 0x64400000u; // 100.64.0.0/10
}

namespace {
// Lower is better.
int addressTier(const QHostAddress& address) {
    if (isPrivateIPv4(address)) {
        return 0; // same Wi-Fi -- what we want
    }
    if (isSharedAddressSpaceIPv4(address)) {
        return 1; // VPN overlay (Tailscale): works, but only on the tailnet
    }
    return 2; // routable: almost certainly not how the phone should connect
}
} // namespace

QList<QHostAddress> rankReachableIPv4(const QList<QHostAddress>& candidates) {
    QList<QHostAddress> result;
    for (const QHostAddress& address : candidates) {
        bool isIPv4 = false;
        address.toIPv4Address(&isIPv4);
        if (!isIPv4) {
            // An IPv6 address is not something anyone wants to type on a phone.
            continue;
        }
        if (address.isLoopback() || address.isLinkLocal()) {
            continue;
        }
        result.append(address);
    }
    std::stable_sort(result.begin(),
            result.end(),
            [](const QHostAddress& a, const QHostAddress& b) {
                return addressTier(a) < addressTier(b);
            });
    return result;
}

QList<QHostAddress> reachableIPv4Addresses() {
    QList<QHostAddress> candidates;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& interface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
                !flags.testFlag(QNetworkInterface::IsRunning) ||
                flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QList<QNetworkAddressEntry> entries = interface.addressEntries();
        for (const QNetworkAddressEntry& entry : entries) {
            candidates.append(entry.ip());
        }
    }
    return rankReachableIPv4(candidates);
}

} // namespace companion
} // namespace mixxx
