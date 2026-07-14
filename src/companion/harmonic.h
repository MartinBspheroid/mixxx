#pragma once

#include <QStringList>

#include "proto/keys.pb.h"

namespace mixxx {
namespace companion {
namespace harmonic {

/// How a candidate track's key relates to the currently playing key, in
/// Camelot/Open-Key wheel terms. Relative major/minor share a wheel number
/// (Camelot 8A/8B), so the wheel position plus the mode fully determines this.
enum class KeyRelation {
    Unknown,      ///< either key is missing/invalid
    Same,         ///< same wheel number and mode (8A -> 8A)
    RelativeMode, ///< same number, other mode (8A -> 8B)
    Adjacent,     ///< +-1 on the wheel, same mode (8A -> 7A/9A)
    Incompatible, ///< anything else
};

/// True for C_MAJOR..B_MAJOR (1..12); minors are 13..24.
bool isMajor(track::io::key::ChromaticKey key);

/// Classify the harmonic relationship between two keys. Pure.
KeyRelation relation(
        track::io::key::ChromaticKey from, track::io::key::ChromaticKey to);

/// Short human tag for a relation, shown verbatim by the client
/// ("perfect key", "energy switch", "safe blend", "key clash"). Empty for Unknown.
QString relationTag(KeyRelation relation);

/// Inputs for scoring one candidate against the currently playing track.
struct Candidate {
    double fromBpm = 0.0; ///< effective BPM of the playing deck
    track::io::key::ChromaticKey fromKey = track::io::key::INVALID;
    double toBpm = 0.0;
    track::io::key::ChromaticKey toKey = track::io::key::INVALID;
    bool playedTonight = false;
    int rating = 0; ///< 0..5
};

struct Score {
    double score = 0.0;
    QStringList reasons; ///< verbatim tags for the client
};

/// Score a candidate as a "what can I play next" suggestion. Higher is better.
/// Pure and deterministic, so it is fully unit-testable.
Score scoreCandidate(const Candidate& candidate);

} // namespace harmonic
} // namespace companion
} // namespace mixxx
