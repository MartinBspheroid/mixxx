#pragma once

#include <QJsonObject>

#include "track/track_decl.h"

#ifdef __STEM__
#include <QJsonArray>
#include <QList>

#include "track/steminfo.h"
#endif

namespace mixxx {
namespace companion {

/// Serializes a Track into the `TrackDto` JSON of the Companion API (see
/// docs/companion/03-api-spec.md). Empty/unset fields are omitted; `title` and
/// `artist` are always present. The absolute file path (`location`) is only
/// included when `exposeFilePaths` is true.
///
/// MUST be called on the main thread: it reads Track properties that are guarded
/// by the global track cache / main-thread ownership model.
QJsonObject serializeTrack(const TrackPointer& pTrack, bool exposeFilePaths);

/// Serializes a track's beat grid into the `BeatgridDto` JSON of the Companion
/// API: `{bpm?, constantTempo?, beats:[seconds], truncated?}`. `beats` is always
/// present -- empty for a track with no grid yet -- and is capped at
/// `kMaxBeatgridBeats` entries, in which case `truncated` is true.
///
/// The caller adds the framing (`trackId` for the HTTP DTO, `deck`/`generation`
/// for the `deck.beatgrid` event).
///
/// MUST be called on the main thread, for the same reason as serializeTrack().
QJsonObject serializeBeatgrid(const TrackPointer& pTrack);

/// Payload bound for serializeBeatgrid(); even a 10-minute 200 BPM track only
/// has ~2000 beats.
constexpr int kMaxBeatgridBeats = 4096;

/// Serializes a track's cue points into the `CuesDto` JSON of the Companion
/// API: `{cues:[{type, positionSeconds?, lengthSeconds?, index?, label?, color}]}`.
/// `cues` is always present. Cue types the API does not expose
/// (Invalid/Beat/Jump/N60dBSound) are skipped, as are cues on a track with no
/// known sample rate, whose positions cannot be expressed in seconds.
///
/// The caller adds the framing (`trackId` for the HTTP DTO, `deck`/`generation`
/// for the `deck.cues` event).
///
/// MUST be called on the main thread, for the same reason as serializeTrack().
QJsonObject serializeCues(const TrackPointer& pTrack);

#ifdef __STEM__
/// Serializes a stem track's static per-stem metadata into a JSON array of
/// `{label, color?}`, in stem order (index 0..N-1). Live per-stem volume/mute
/// state is not here -- it streams in `deck.tick` and is joined by index. Only
/// meaningful for stem tracks (`Track::hasStem()`); an empty list yields `[]`.
QJsonArray serializeStems(const QList<StemInfo>& stems);
#endif

} // namespace companion
} // namespace mixxx
