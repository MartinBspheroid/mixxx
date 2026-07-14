#include <gtest/gtest.h>

#include "companion/harmonic.h"
#include "track/keyutils.h"

using namespace mixxx::companion::harmonic;
namespace key = mixxx::track::io::key;

namespace {

// Camelot 8A == A minor, 8B == C major (they share Open-Key number 1).
// Camelot 9A == E minor, 7A == D minor (Open-Key 2 and 12 respectively).

TEST(CompanionHarmonicTest, MajorMinorClassification) {
    EXPECT_TRUE(isMajor(key::C_MAJOR));
    EXPECT_TRUE(isMajor(key::B_MAJOR));
    EXPECT_FALSE(isMajor(key::A_MINOR));
    EXPECT_FALSE(isMajor(key::B_MINOR));
}

TEST(CompanionHarmonicTest, SameKeyIsPerfect) {
    EXPECT_EQ(relation(key::A_MINOR, key::A_MINOR), KeyRelation::Same);
    EXPECT_EQ(relationTag(KeyRelation::Same), QStringLiteral("perfect key"));
}

TEST(CompanionHarmonicTest, RelativeMajorMinorSharesWheelNumber) {
    // A minor and C major are relatives (both Open-Key 1 / Camelot 8).
    ASSERT_EQ(KeyUtils::keyToOpenKeyNumber(key::A_MINOR),
            KeyUtils::keyToOpenKeyNumber(key::C_MAJOR));
    EXPECT_EQ(relation(key::A_MINOR, key::C_MAJOR), KeyRelation::RelativeMode);
    EXPECT_EQ(relation(key::C_MAJOR, key::A_MINOR), KeyRelation::RelativeMode);
    EXPECT_EQ(relationTag(KeyRelation::RelativeMode),
            QStringLiteral("energy switch"));
}

TEST(CompanionHarmonicTest, AdjacentOnWheelSameMode) {
    // A minor (OpenKey 1) -> E minor (OpenKey 2) is +1 on the wheel, same mode.
    EXPECT_EQ(relation(key::A_MINOR, key::E_MINOR), KeyRelation::Adjacent);
    // C major (1) -> G major (2) likewise.
    EXPECT_EQ(relation(key::C_MAJOR, key::G_MAJOR), KeyRelation::Adjacent);
    EXPECT_EQ(relationTag(KeyRelation::Adjacent), QStringLiteral("safe blend"));
}

TEST(CompanionHarmonicTest, WheelWrapsAround) {
    // OpenKey 1 -> 12 is adjacent across the wrap (distance 1, not 11).
    const auto twelve = KeyUtils::openKeyNumberToKey(12, /*major*/ false);
    const auto one = KeyUtils::openKeyNumberToKey(1, /*major*/ false);
    EXPECT_EQ(relation(one, twelve), KeyRelation::Adjacent);
    EXPECT_EQ(relation(twelve, one), KeyRelation::Adjacent);
}

TEST(CompanionHarmonicTest, DistantKeysClash) {
    // OpenKey 1 -> 6 (same mode) is far apart.
    const auto one = KeyUtils::openKeyNumberToKey(1, false);
    const auto six = KeyUtils::openKeyNumberToKey(6, false);
    EXPECT_EQ(relation(one, six), KeyRelation::Incompatible);
    EXPECT_EQ(relationTag(KeyRelation::Incompatible), QStringLiteral("key clash"));
}

TEST(CompanionHarmonicTest, InvalidKeysAreUnknown) {
    EXPECT_EQ(relation(key::INVALID, key::A_MINOR), KeyRelation::Unknown);
    EXPECT_EQ(relation(key::A_MINOR, key::INVALID), KeyRelation::Unknown);
    EXPECT_TRUE(relationTag(KeyRelation::Unknown).isEmpty());
}

TEST(CompanionHarmonicTest, PerfectMatchScoresHighAndTagsIt) {
    Candidate c;
    c.fromBpm = 126.0;
    c.fromKey = key::A_MINOR;
    c.toBpm = 126.5;
    c.toKey = key::A_MINOR;
    const Score s = scoreCandidate(c);
    // same key (40) + tempo match (30)
    EXPECT_GT(s.score, 60.0);
    EXPECT_TRUE(s.reasons.contains(QStringLiteral("perfect key")));
    EXPECT_TRUE(s.reasons.contains(QStringLiteral("tempo match")));
}

TEST(CompanionHarmonicTest, BigBpmJumpIsPenalizedAndTagged) {
    Candidate c;
    c.fromBpm = 120.0;
    c.fromKey = key::A_MINOR;
    c.toBpm = 140.0; // +20
    c.toKey = key::A_MINOR;
    const Score s = scoreCandidate(c);
    EXPECT_TRUE(s.reasons.contains(QStringLiteral("big BPM jump")));
    // same key (+40) - jump (25) + energy tag; still below a perfect match
    Candidate perfect = c;
    perfect.toBpm = 120.0;
    EXPECT_LT(s.score, scoreCandidate(perfect).score);
}

TEST(CompanionHarmonicTest, PlayedTonightIsHeavilyPenalized) {
    Candidate fresh;
    fresh.fromBpm = 126.0;
    fresh.fromKey = key::A_MINOR;
    fresh.toBpm = 126.0;
    fresh.toKey = key::A_MINOR;

    Candidate repeat = fresh;
    repeat.playedTonight = true;

    const Score freshScore = scoreCandidate(fresh);
    const Score repeatScore = scoreCandidate(repeat);
    EXPECT_TRUE(repeatScore.reasons.contains(QStringLiteral("already played")));
    EXPECT_LT(repeatScore.score, freshScore.score);
    // A played track must never outrank even a key-clashing fresh one.
    Candidate clash = fresh;
    clash.toKey = KeyUtils::openKeyNumberToKey(6, false);
    EXPECT_LT(repeatScore.score, scoreCandidate(clash).score);
}

TEST(CompanionHarmonicTest, EnergyDirectionTags) {
    Candidate up;
    up.fromBpm = 124.0;
    up.toBpm = 128.0;
    up.fromKey = key::A_MINOR;
    up.toKey = key::A_MINOR;
    EXPECT_TRUE(scoreCandidate(up).reasons.contains(QStringLiteral("+1 energy")));

    Candidate down = up;
    down.toBpm = 120.0;
    EXPECT_TRUE(scoreCandidate(down).reasons.contains(QStringLiteral("-1 energy")));

    Candidate flat = up;
    flat.toBpm = 124.0;
    const QStringList flatReasons = scoreCandidate(flat).reasons;
    EXPECT_FALSE(flatReasons.contains(QStringLiteral("+1 energy")));
    EXPECT_FALSE(flatReasons.contains(QStringLiteral("-1 energy")));
}

TEST(CompanionHarmonicTest, RatingNudgesScore) {
    Candidate base;
    base.fromBpm = 126.0;
    base.toBpm = 126.0;
    base.fromKey = key::A_MINOR;
    base.toKey = key::A_MINOR;

    Candidate rated = base;
    rated.rating = 5;
    EXPECT_GT(scoreCandidate(rated).score, scoreCandidate(base).score);
}

} // namespace
