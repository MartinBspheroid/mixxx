#pragma once

#include <QByteArray>

class Waveform;

namespace mixxx {
namespace companion {

/// Encode a summary waveform as an `MXWF v1` binary blob (see
/// docs/companion/03-api-spec.md). Pure function over the Waveform accessor
/// interface so it is unit-testable without a database or a loaded track.
///
/// Layout: 32-byte little-endian header (magic "MXWF", version, flags, trackId,
/// durationSeconds f32, sampleCount = getDataSize()/2 visual frames, channels=1,
/// bands=4, reserved) followed by `sampleCount * 4` bytes, per visual frame the
/// L/R max-mixed bands in order [all, low, mid, high].
QByteArray encodeWaveformSummaryMxwf(
        quint32 trackId, const Waveform& waveform, double durationSeconds);

} // namespace companion
} // namespace mixxx
