#include <gtest/gtest.h>

#include <QHostAddress>
#include <QList>

#include "companion/networkinfo.h"

using mixxx::companion::isPrivateIPv4;
using mixxx::companion::isSharedAddressSpaceIPv4;
using mixxx::companion::rankReachableIPv4;

namespace {

QList<QHostAddress> addresses(const QStringList& raw) {
    QList<QHostAddress> result;
    for (const QString& one : raw) {
        result.append(QHostAddress(one));
    }
    return result;
}

QStringList toStrings(const QList<QHostAddress>& in) {
    QStringList result;
    for (const QHostAddress& address : in) {
        result.append(address.toString());
    }
    return result;
}

TEST(CompanionNetworkInfoTest, RecognizesTheRfc1918Ranges) {
    EXPECT_TRUE(isPrivateIPv4(QHostAddress(QStringLiteral("10.0.0.5"))));
    EXPECT_TRUE(isPrivateIPv4(QHostAddress(QStringLiteral("192.168.1.42"))));
    EXPECT_TRUE(isPrivateIPv4(QHostAddress(QStringLiteral("172.16.0.1"))));
    EXPECT_TRUE(isPrivateIPv4(QHostAddress(QStringLiteral("172.31.255.254"))));

    // Just outside 172.16/12 on both sides.
    EXPECT_FALSE(isPrivateIPv4(QHostAddress(QStringLiteral("172.15.0.1"))));
    EXPECT_FALSE(isPrivateIPv4(QHostAddress(QStringLiteral("172.32.0.1"))));
    // 11/8 and 193.168/16 are near-misses for 10/8 and 192.168/16.
    EXPECT_FALSE(isPrivateIPv4(QHostAddress(QStringLiteral("11.0.0.5"))));
    EXPECT_FALSE(isPrivateIPv4(QHostAddress(QStringLiteral("193.168.1.42"))));
    EXPECT_FALSE(isPrivateIPv4(QHostAddress(QStringLiteral("95.217.114.51"))));
}

TEST(CompanionNetworkInfoTest, DropsLoopbackAndLinkLocal) {
    const auto ranked = rankReachableIPv4(addresses({QStringLiteral("127.0.0.1"),
            QStringLiteral("169.254.3.4"),
            QStringLiteral("192.168.1.42")}));
    EXPECT_EQ(toStrings(ranked), QStringList{QStringLiteral("192.168.1.42")});
}

TEST(CompanionNetworkInfoTest, DropsIPv6) {
    // Nobody is typing this into a phone.
    const auto ranked = rankReachableIPv4(addresses({QStringLiteral("fe80::1"),
            QStringLiteral("2001:db8::1"),
            QStringLiteral("10.0.0.5")}));
    EXPECT_EQ(toStrings(ranked), QStringList{QStringLiteral("10.0.0.5")});
}

// A machine with a public address is exactly where guessing goes wrong: the
// phone is on the LAN, and advertising the routable address is both useless
// and the wrong thing to put in front of the user.
TEST(CompanionNetworkInfoTest, PrivateAddressesOutrankPublicOnes) {
    const auto ranked = rankReachableIPv4(
            addresses({QStringLiteral("95.217.114.51"),
                    QStringLiteral("192.168.1.42")}));
    ASSERT_EQ(ranked.size(), 2);
    EXPECT_EQ(ranked.first().toString(), QStringLiteral("192.168.1.42"));
}

// Two LAN interfaces (ethernet + Wi-Fi) are equally plausible, so the order the
// system reported them in is preserved and the page offers both.
TEST(CompanionNetworkInfoTest, EqualCandidatesKeepInterfaceOrder) {
    const auto ranked = rankReachableIPv4(addresses({QStringLiteral("10.0.0.5"),
            QStringLiteral("192.168.1.42")}));
    EXPECT_EQ(toStrings(ranked),
            (QStringList{QStringLiteral("10.0.0.5"),
                    QStringLiteral("192.168.1.42")}));
}

TEST(CompanionNetworkInfoTest, RecognizesSharedAddressSpace) {
    // 100.64/10 is what Tailscale hands out.
    EXPECT_TRUE(isSharedAddressSpaceIPv4(
            QHostAddress(QStringLiteral("100.70.123.25"))));
    EXPECT_TRUE(
            isSharedAddressSpaceIPv4(QHostAddress(QStringLiteral("100.64.0.0"))));
    EXPECT_TRUE(isSharedAddressSpaceIPv4(
            QHostAddress(QStringLiteral("100.127.255.255"))));
    // Just outside the /10 on both sides.
    EXPECT_FALSE(isSharedAddressSpaceIPv4(
            QHostAddress(QStringLiteral("100.63.255.255"))));
    EXPECT_FALSE(
            isSharedAddressSpaceIPv4(QHostAddress(QStringLiteral("100.128.0.1"))));
    // It is not RFC1918, and must not be mistaken for it.
    EXPECT_FALSE(isPrivateIPv4(QHostAddress(QStringLiteral("100.70.123.25"))));
}

// A tailnet address does reach the phone, so it beats a routable one -- but the
// Wi-Fi the phone is actually on beats both.
TEST(CompanionNetworkInfoTest, TailscaleRanksBelowLanAndAbovePublic) {
    const auto ranked =
            rankReachableIPv4(addresses({QStringLiteral("95.217.114.51"),
                    QStringLiteral("100.70.123.25"),
                    QStringLiteral("192.168.1.42")}));
    EXPECT_EQ(toStrings(ranked),
            (QStringList{QStringLiteral("192.168.1.42"),
                    QStringLiteral("100.70.123.25"),
                    QStringLiteral("95.217.114.51")}));
}

TEST(CompanionNetworkInfoTest, NoUsableAddressYieldsEmpty) {
    const auto ranked = rankReachableIPv4(addresses(
            {QStringLiteral("127.0.0.1"), QStringLiteral("169.254.3.4")}));
    EXPECT_TRUE(ranked.isEmpty());
}

} // namespace
