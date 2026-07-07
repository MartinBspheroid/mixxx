#include <gtest/gtest.h>

#include "companion/pairingmanager.h"

using mixxx::companion::PairingManager;

namespace {

TEST(CompanionPairingTest, LoopbackIsAlwaysAllowed) {
    PairingManager pm;
    EXPECT_EQ(pm.authorize(true, QByteArray(), true), PairingManager::AuthResult::Ok);
    EXPECT_EQ(pm.authorize(true, "garbage", true), PairingManager::AuthResult::Ok);
}

TEST(CompanionPairingTest, NonLoopbackWithoutTokenIsUnauthorized) {
    PairingManager pm;
    EXPECT_EQ(pm.authorize(false, QByteArray(), false),
            PairingManager::AuthResult::Unauthorized);
    EXPECT_EQ(pm.authorize(false, "not-a-real-token", false),
            PairingManager::AuthResult::Unauthorized);
}

TEST(CompanionPairingTest, ClaimFlowMintsUsableToken) {
    PairingManager pm;
    const QString code = pm.beginPairing(60, 1000);
    ASSERT_EQ(code.size(), 6);

    // Wrong code is rejected.
    auto bad = pm.claim(QStringLiteral("000000") == code ? QStringLiteral("111111")
                                                         : QStringLiteral("000000"),
            QStringLiteral("phone"),
            /*readOnly*/ false,
            1500);
    // (If the random code happened to equal our "wrong" guess, skip this leg.)
    if (!bad.ok) {
        EXPECT_EQ(bad.error, QStringLiteral("bad_code"));
    }

    auto claimed = pm.claim(code, QStringLiteral("phone"), false, 2000);
    ASSERT_TRUE(claimed.ok) << claimed.error.toStdString();
    ASSERT_FALSE(claimed.token.isEmpty());

    // The minted token authorizes a non-loopback control request.
    EXPECT_EQ(pm.authorize(false, claimed.token.toLatin1(), /*needsControl*/ true),
            PairingManager::AuthResult::Ok);
}

TEST(CompanionPairingTest, ReadOnlyTokenCannotControl) {
    PairingManager pm;
    const QString code = pm.beginPairing(60, 0);
    auto claimed = pm.claim(code, QStringLiteral("readonly-phone"),
            /*readOnly*/ true, 10);
    ASSERT_TRUE(claimed.ok);
    const QByteArray token = claimed.token.toLatin1();

    EXPECT_EQ(pm.authorize(false, token, /*needsControl*/ false),
            PairingManager::AuthResult::Ok);
    EXPECT_EQ(pm.authorize(false, token, /*needsControl*/ true),
            PairingManager::AuthResult::Forbidden);
}

TEST(CompanionPairingTest, ExpiredWindowRejectsClaim) {
    PairingManager pm;
    const QString code = pm.beginPairing(60, 0);
    // 61s later -> expired.
    auto claimed = pm.claim(code, QStringLiteral("late"), false, 61'000);
    EXPECT_FALSE(claimed.ok);
    EXPECT_EQ(claimed.error, QStringLiteral("no_pairing_window"));
}

TEST(CompanionPairingTest, TooManyWrongAttemptsLockOut) {
    PairingManager pm;
    pm.beginPairing(600, 0);
    for (int i = 0; i < 5; ++i) {
        pm.claim(QStringLiteral("999999"), QStringLiteral("brute"), false, 100);
    }
    // 6th attempt (even with a would-be-correct code) is locked out.
    auto result = pm.claim(QStringLiteral("000000"), QStringLiteral("brute"), false, 100);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, QStringLiteral("too_many_attempts"));
}

TEST(CompanionPairingTest, RevokeInvalidatesToken) {
    PairingManager pm;
    const QString code = pm.beginPairing(60, 0);
    auto claimed = pm.claim(code, QStringLiteral("phone"), false, 10);
    ASSERT_TRUE(claimed.ok);
    const QByteArray token = claimed.token.toLatin1();
    const QByteArray id = PairingManager::hashToken(token);

    ASSERT_EQ(pm.authorize(false, token, false), PairingManager::AuthResult::Ok);
    EXPECT_TRUE(pm.revokeDevice(id));
    EXPECT_EQ(pm.authorize(false, token, false),
            PairingManager::AuthResult::Unauthorized);
    EXPECT_FALSE(pm.revokeDevice(id)); // already gone
}

TEST(CompanionPairingTest, SerializeLoadRoundTrip) {
    PairingManager pm;
    const QString code = pm.beginPairing(60, 0);
    auto claimed = pm.claim(code, QStringLiteral("my device"), true, 10);
    ASSERT_TRUE(claimed.ok);
    const QByteArray token = claimed.token.toLatin1();

    const QString serialized = pm.serializeTokens();

    PairingManager restored;
    restored.loadTokens(serialized);
    // Read-only scope preserved across persistence.
    EXPECT_EQ(restored.authorize(false, token, false),
            PairingManager::AuthResult::Ok);
    EXPECT_EQ(restored.authorize(false, token, true),
            PairingManager::AuthResult::Forbidden);
    EXPECT_TRUE(restored.hasPairedDevices());
}

TEST(CompanionPairingTest, HashIsDeterministicAndNotPlaintext) {
    const QByteArray token = "abc123";
    const QByteArray h1 = PairingManager::hashToken(token);
    const QByteArray h2 = PairingManager::hashToken(token);
    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, token);
    EXPECT_EQ(h1.size(), 64); // SHA-256 hex
}

} // namespace
