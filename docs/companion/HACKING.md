# Companion API — Developer Guide

How to build, enable, and poke at the Companion API during development. For the
protocol contract see `03-api-spec.md`; for the design see `02-architecture.md`.

## Build

The service is compiled in by default (`COMPANION_API=ON`). It adds a dependency
on Qt's `WebSockets` module — make sure your Qt/vcpkg environment provides
`qtwebsockets`. To build without it:

```sh
cmake -B build -DCOMPANION_API=OFF ...
```

When `COMPANION_API=ON`, the compile definition `__COMPANION__` is set and the
`src/companion/` sources are added to `mixxx-lib`.

## Enable at runtime

The service is off unless enabled in the config. Edit `mixxx.cfg` (in your Mixxx
settings dir) and add a `[CompanionAPI]` group:

```ini
[CompanionAPI]
enabled = 1
port = 24742
allow_lan = 0
tick_interval_ms = 100
expose_file_paths = 0
```

- `enabled` — master switch (default `0`).
- `port` — TCP port for HTTP + WebSocket (default `24742`).
- `allow_lan` — `0` binds `127.0.0.1` only; `1` binds all interfaces. Until
  pairing (T08) lands, `allow_lan=1` exposes an **unauthenticated** API — only use
  it on a trusted network.
- `tick_interval_ms` — deck tick cadence, clamped to 50–200 ms.
- `expose_file_paths` — include absolute file paths in track metadata (default
  `0`; treat paths as privileged).

Restart Mixxx after editing. On start you should see a log line:

```
Companion - Companion API listening on 127.0.0.1 port 24742
```

Companion logs use the `Companion` logger context; filter with Mixxx's logging
controls (e.g. `--logLevel debug`).

## Poke at it

```sh
# Status
curl -s http://127.0.0.1:24742/v1/status | jq

# Live deck events while you load/play tracks in Mixxx
websocat ws://127.0.0.1:24742/ws/v1
#   → deck.loaded / deck.tick / deck.seek / deck.unloaded
#   On connect you get a replay of the currently loaded decks.

# Unknown route -> 404 JSON error
curl -s -i http://127.0.0.1:24742/v1/nope

# Remote actions (T07) — "call functions from the phone"
curl -s -X POST http://127.0.0.1:24742/v1/decks/1/play
curl -s -X POST http://127.0.0.1:24742/v1/decks/1/pause
curl -s -X POST http://127.0.0.1:24742/v1/decks/1/cue
curl -s -X POST http://127.0.0.1:24742/v1/decks/1/sync
curl -s -X POST http://127.0.0.1:24742/v1/decks/1/seek -d '{"position":0.25}'
curl -s -X POST http://127.0.0.1:24742/v1/decks/1/load -d '{"trackId":123,"play":false}'
#   load is fire-and-forget (202); watch /ws/v1 for the deck.loaded confirmation
```

No `websocat`? A three-line Node or Python client works too — see
`CLIENT.md` (added in T10) for reference snippets.

## Architecture recap (where to look)

| Concern | File |
|---|---|
| Lifecycle, deck signal wiring, thread mgmt | `src/companion/companionservice.cpp` (main thread) |
| Listeners, client registry, routing, replay | `src/companion/companionserver.cpp` (worker thread) |
| HTTP parse / WebSocket-upgrade classification | `src/companion/httpconnection.cpp` |
| Deck tick polling of the control system | `src/companion/deckstatepublisher.cpp` |
| Track → JSON | `src/companion/trackserializer.cpp` |
| Config keys | `src/companion/companionsettings.h` |
| Wire-in point | `src/coreservices.cpp` (`__COMPANION__` blocks) |

## Threading rules (do not violate)

- Everything under `src/companion/` except `CompanionService` itself runs on the
  `CompanionAPI` worker thread.
- Deck/library reads that touch Mixxx objects happen on the main thread inside
  `CompanionService`, which then sends serialized JSON to the worker via queued
  signals. `ControlObject::get()` is the one exception — it is thread-safe and is
  read directly from the worker by `DeckStatePublisher`.
- Never block the audio engine; never touch DAOs off the main thread.
