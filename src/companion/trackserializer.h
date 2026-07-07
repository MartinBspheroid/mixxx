#pragma once

#include <QJsonObject>

#include "track/track_decl.h"

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

} // namespace companion
} // namespace mixxx
