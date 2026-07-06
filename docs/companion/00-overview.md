# 00 — Overview and Decision Log

## Vision

Turn Mixxx into a hackable DJ engine with a first-class companion API, so a phone or
tablet becomes the *eyes and brain* of a DJ setup while physical hardware stays the
*hands* and Mixxx stays the *audio engine*:

```
hardware = hands        (play/cue/jog/EQ/loops — muscle memory, low latency)
Mixxx    = audio engine
phone    = eyes + brain (song info, search, prep queue, suggestions, HUD)
companion service = nervous system
```

The companion client is **never** in the audio-critical path. No scratching, no
cueing, no beatmatching from the phone. If the phone disconnects mid-set, nothing
about the performance breaks.

## Why a fork (and why the fork stays narrow)

Investigated alternatives, all rejected for the primary integration layer:

- **Controller scripting (JS)** — deck state via controls works, but rich loaded-track
  metadata is not available to MIDI scripts (upstream issue #13652 closed "not
  planned"), no library search, no waveform access, no sockets. Scripts remain a
  *client* of the system, not its foundation.
- **External SQLite scraping** — `mixxxdb.sqlite` is internal, migration-managed, and
  main-thread-owned inside Mixxx. Brittle; prototype-only.
- **Sidecar generating its own waveforms/metadata** — viable as MVP fallback, but we
  own the engine source, so exposing real state natively is cleaner and no harder.

Therefore: **fork adds one narrow module, `src/companion/`**, instantiated by
`CoreServices`. No engine changes, no UI rewrite, no library schema changes. The
shape is deliberately upstreamable later (read-only local companion API).

## What Mixxx already gives us (verified on this branch — see 01-codebase-map.md)

| Need | Mixxx internal | Exposed externally today |
|---|---|---|
| Deck play state, position, BPM, key, VU | Control system (`ControlObject`/`ControlProxy`) | No (only to controllers) |
| Track load/unload events | `BaseTrackPlayer` signals | No |
| Loaded track metadata | `Track` getters | No |
| Waveform summary (2×1920 samples) + main waveform (441 samples/s) | `AnalyzerWaveform` → `AnalysisDao` / `Track::getWaveformSummary()` | No |
| Library search | `SearchQueryParser`, `BaseTrackCache`, DAOs | No |
| Load track to deck | `PlayerManager` slots | No |
| Network server of any kind | — none in tree — | No |

## Architecture summary

```
Mixxx internals
  ├─ PlayerManager / BaseTrackPlayer     (load events, deck lookup, load actions)
  ├─ ControlObject / ControlProxy        (play, playposition, duration, rate, bpm, key, vu)
  ├─ TrackCollectionManager / DAOs       (library, search, playlists, crates, cues)
  ├─ Track / AnalysisDao                 (metadata, waveform blobs)
  ▼
CompanionService (src/companion/, own QThread, Qt signals in, sockets out)
  ├─ WebSocket  /ws/v1     — live deck state events (push)
  ├─ HTTP       /v1/...    — request/response: status, tracks, search, waveforms
  └─ Pairing / token auth  — default bind 127.0.0.1; LAN opt-in
  ▼
Clients: Android app (Kotlin + Compose), browser dashboard, ESP32 later
```

Transport decision: **WebSocket + HTTP over Wi-Fi/LAN. Not Bluetooth.** Reasons:
payload sizes (waveforms are tens of KB), reconnect semantics, works in browsers and
Android immediately, binary frames, debuggability. BLE/MIDI may appear later for
physical controls only.

Sync model: Mixxx is authoritative. Service pushes `deck.tick` at **10–20 Hz**;
client extrapolates position locally (`position + elapsed × rate`) and renders at
60 FPS; corrections arrive with each tick; seeks push an immediate packet. Every
deck message carries a **generation counter** so a stale packet can never paint the
wrong track's data.

## MVP (maps to tasks T03–T07)

1. `GET /v1/status` — service alive, version, deck count.
2. WS `deck.loaded` / `deck.unloaded` / `deck.tick` with full song info.
3. `GET /v1/library/search?q=...&bpmMin=&bpmMax=&key=` — lightweight track rows.
4. `POST /v1/decks/:deck/load` + transport controls — remote function calls.

Explicitly **not** MVP: waveform export (T09), artwork, playlist/crate browsing,
prep queue, suggestions, session history, any AI features, skins, controller
mappings. These are phased behind the MVP (see roadmap below).

## Roadmap (from the original plan, adapted to fork-first reality)

- **Phase 1 — Communication + HUD data** (T01–T05): service boots, status endpoint,
  deck events with song info. Proves the bridge.
- **Phase 2 — Library + actions** (T06–T08): search, load-to-deck, transport
  controls, pairing for LAN use. Phone becomes useful.
- **Phase 3 — Waveform + richer data** (T09): overview waveform, cue/loop markers,
  playlists/crates/history endpoints.
- **Phase 4 — Clients** (T10): Android app (Live HUD, Library, Queue screens).
- **Phase 5 — Intelligence** (later): prep queue, "what can I play next"
  suggestions (BPM/key/energy/history) — the killer feature; smart crates; session
  recorder. All client/bridge-side; no further engine surface needed.

## Decision log

| # | Decision | Rationale |
|---|---|---|
| D1 | Fork Mixxx 2.6 branch, add `src/companion/` only | 2.6-beta has stems, controller-screen rendering (prior art), QML groundwork; narrow diff eases rebasing on upstream 2.6 → 2.6.x |
| D2 | WebSocket for live state, HTTP for request/response | Push vs pull semantics; binary waveform blobs over HTTP GET |
| D3 | Qt-only implementation (QtWebSockets; QtHttpServer or minimal QTcpServer HTTP) | Cross-platform for free (macOS/Linux/RasPi); no new C++ deps to port |
| D4 | Read-only first; write actions gated and enumerated | Safety; matches "hard rules"; T07 whitelist only |
| D5 | Default bind 127.0.0.1; LAN requires explicit toggle + pairing token | A DJ app that lets anything on Wi-Fi load tracks is a bad idea |
| D6 | Send waveform *data* once per load, never rendered frames or streams | 60 FPS rendering belongs to the client; summary is ~30 KB |
| D7 | Phone is non-critical; no low-latency control path | Role split above |
| D8 | JSON for events/DTOs; explicit little-endian binary for waveforms | Debuggable; endianness-safe across arm64/x86_64 |
| D9 | Android client: native Kotlin + Jetpack Compose (separate repo) | LAN discovery, wake locks, tablet layouts, background sockets age better than RN/PWA |
| D10 | Docs-first: API spec updated in the same PR as code | "Thorough documentation" is a stated project goal |
