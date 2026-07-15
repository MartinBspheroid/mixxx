#pragma once

#include <QHostAddress>
#include <QList>

namespace mixxx {
namespace companion {

/// True for the RFC1918 private IPv4 ranges (10/8, 172.16/12, 192.168/16) --
/// the ones a phone on the same Wi-Fi is actually on.
bool isPrivateIPv4(const QHostAddress& address);

/// True for RFC6598 shared address space (100.64/10), which in practice means a
/// VPN overlay such as Tailscale. Reachable from a phone on the same tailnet,
/// but only after the LAN proper.
bool isSharedAddressSpaceIPv4(const QHostAddress& address);

/// Reduce a pile of interface addresses to the IPv4 ones worth showing a user,
/// best first: LAN (RFC1918), then VPN overlay (100.64/10), then anything else.
/// A DJ laptop is on a club/home network, so a public address is both useless to
/// the phone and the wrong thing to advertise. Loopback and link-local
/// (169.254/16) are dropped; relative order within a tier is preserved.
///
/// Pure, so the ranking is unit-testable without real network interfaces.
QList<QHostAddress> rankReachableIPv4(const QList<QHostAddress>& candidates);

/// This machine's addresses, ranked by rankReachableIPv4(). Only interfaces
/// that are up and running are considered.
QList<QHostAddress> reachableIPv4Addresses();

} // namespace companion
} // namespace mixxx
