#pragma once

#include <QtGlobal>

/// Shared constants for the Companion API.
///
/// The Companion API is an optional local service (built only when the
/// COMPANION_API CMake option is enabled, which defines `__COMPANION__`) that
/// exposes deck state, loaded-track metadata, library search and a small set of
/// safe remote actions to companion clients over HTTP + WebSocket on the local
/// network. See docs/companion/ for the full design.
namespace mixxx {
namespace companion {

/// Wire protocol version. Reflected in `GET /v1/status` as `apiVersion` and in
/// the versioned URL prefixes (`/v1/...`, `/ws/v1`). A breaking change bumps
/// this and introduces a `/v2` surface.
constexpr int kApiVersion = 1;

/// Default TCP port shared by the HTTP and WebSocket endpoints.
constexpr quint16 kDefaultPort = 24742;

/// Deck tick cadence bounds (milliseconds). The publisher pushes deck.tick
/// events at this period while state changes, clamped to this range.
constexpr int kMinTickIntervalMs = 50;
constexpr int kMaxTickIntervalMs = 200;
constexpr int kDefaultTickIntervalMs = 100;

} // namespace companion
} // namespace mixxx
