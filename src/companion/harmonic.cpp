#include "companion/harmonic.h"

#include <cmath>

#include "track/keyutils.h"

namespace {
// Camelot/Open-Key wheel positions (1..12). Relative major/minor share a number.
constexpr int kWheelSize = 12;

// Shortest distance between two wheel positions (0..6).
int wheelDistance(int a, int b) {
    const int raw = std::abs(a - b);
    return std::min(raw, kWheelSize - raw);
}

// Tempo scoring thresholds (BPM difference).
constexpr double kTempoPerfect = 2.0;
constexpr double kTempoClose = 4.0;
constexpr double kTempoStretch = 6.0;
constexpr double kTempoJump = 8.0;

// Below this BPM delta the tracks are treated as the same energy.
constexpr double kEnergyDeadband = 1.0;
} // namespace

namespace mixxx {
namespace companion {
namespace harmonic {

using track::io::key::ChromaticKey;

bool isMajor(ChromaticKey key) {
    return key >= track::io::key::C_MAJOR && key <= track::io::key::B_MAJOR;
}

KeyRelation relation(ChromaticKey from, ChromaticKey to) {
    if (from == track::io::key::INVALID || to == track::io::key::INVALID) {
        return KeyRelation::Unknown;
    }
    const int fromNumber = KeyUtils::keyToOpenKeyNumber(from);
    const int toNumber = KeyUtils::keyToOpenKeyNumber(to);
    if (fromNumber <= 0 || toNumber <= 0) {
        return KeyRelation::Unknown;
    }
    const bool sameMode = isMajor(from) == isMajor(to);

    if (fromNumber == toNumber) {
        return sameMode ? KeyRelation::Same : KeyRelation::RelativeMode;
    }
    if (sameMode && wheelDistance(fromNumber, toNumber) == 1) {
        return KeyRelation::Adjacent;
    }
    return KeyRelation::Incompatible;
}

QString relationTag(KeyRelation relation) {
    switch (relation) {
    case KeyRelation::Same:
        return QStringLiteral("perfect key");
    case KeyRelation::RelativeMode:
        return QStringLiteral("energy switch");
    case KeyRelation::Adjacent:
        return QStringLiteral("safe blend");
    case KeyRelation::Incompatible:
        return QStringLiteral("key clash");
    case KeyRelation::Unknown:
        break;
    }
    return QString();
}

Score scoreCandidate(const Candidate& candidate) {
    Score result;

    // --- Harmonic key ---
    const KeyRelation rel = relation(candidate.fromKey, candidate.toKey);
    switch (rel) {
    case KeyRelation::Same:
        result.score += 40.0;
        break;
    case KeyRelation::Adjacent:
        result.score += 30.0;
        break;
    case KeyRelation::RelativeMode:
        result.score += 28.0;
        break;
    case KeyRelation::Incompatible:
        result.score -= 25.0;
        break;
    case KeyRelation::Unknown:
        break;
    }
    const QString keyTag = relationTag(rel);
    if (!keyTag.isEmpty()) {
        result.reasons << keyTag;
    }

    // --- Tempo ---
    if (candidate.fromBpm > 0.0 && candidate.toBpm > 0.0) {
        const double delta = std::fabs(candidate.toBpm - candidate.fromBpm);
        if (delta <= kTempoPerfect) {
            result.score += 30.0;
            result.reasons << QStringLiteral("tempo match");
        } else if (delta <= kTempoClose) {
            result.score += 20.0;
        } else if (delta <= kTempoStretch) {
            result.score += 8.0;
            result.reasons << QStringLiteral("slight stretch");
        } else if (delta > kTempoJump) {
            result.score -= 25.0;
            result.reasons << QStringLiteral("big BPM jump");
        }

        // --- Energy (BPM-direction heuristic) ---
        const double signedDelta = candidate.toBpm - candidate.fromBpm;
        if (signedDelta > kEnergyDeadband) {
            result.reasons << QStringLiteral("+1 energy");
        } else if (signedDelta < -kEnergyDeadband) {
            result.reasons << QStringLiteral("-1 energy");
        }
    }

    // --- Freshness ---
    if (candidate.playedTonight) {
        // Heavy penalty: never suggest a repeat above a fresh track.
        result.score -= 100.0;
        result.reasons << QStringLiteral("already played");
    }

    // --- Rating nudge ---
    if (candidate.rating > 0) {
        result.score += candidate.rating * 2.0;
    }

    return result;
}

} // namespace harmonic
} // namespace companion
} // namespace mixxx
