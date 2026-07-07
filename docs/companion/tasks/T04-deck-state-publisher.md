# T04 — WebSocket Server + Deck State Publisher (PRIMARY TARGET)

**Phase:** 1 · **Size:** L · **Blocked by:** T03 (runs in parallel with T05; the
`deck.loaded` track payload uses T05's serializer — stub it with title-only until
T05 merges)

## Objective

`/ws/v1` endpoint pushing `deck.loaded`, `deck.unloaded`, `deck.tick`, `deck.seek`
per `03-api-spec.md`. This is the heart of the project: live song information on
the phone.

## Design references

`02-architecture.md` §Event pipeline and §Threading (read both twice);
`01-codebase-map.md` §1–2 for the exact signals/controls.

## Steps

1. **WebSocket upgrade**: `QWebSocketServer` (NonSecureMode) on the same port if the
   HTTP layer allows socket handover, otherwise port+1 (`24743`) — decide, document
   in the spec's Versioning section, and record as D12. Simplest robust route with
   the hand-rolled HTTP layer: peek the first bytes / `Upgrade` header and hand the
   socket to `QWebSocketServer::handleConnection()`.
2. **Client registry** in `CompanionServer`: track connected `QWebSocket*`,
   handle `hello`/`subscribe`/`ping` messages, update `clients` in `/v1/status`.
3. **Deck signal wiring (main thread, in `CompanionService`)**:
   - For each deck `i` in `0..numberOfDecks-1`: `getDeckBase(i)` → connect
     `newTrackLoaded`, `trackUnloaded`, `loadingTrack`, `playerEmpty`.
   - Handle `numberOfDecksChanged` (connect newly added decks).
   - On load/unload: bump `generation[deck]`, build the event DTO (track payload
     via TrackSerializer), queued-signal to the worker for broadcast.
4. **DeckStatePublisher (worker thread)**:
   - Per deck, `ControlObject::get()` on a `QTimer` at `tick_interval_ms` (default
     100 ms) for: `playposition`, `duration`, `rate`, `play`, `vu_meter`.
     Group strings via `PlayerManager::groupForDeck(i)` passed in at setup.
   - Emit `deck.tick` only when values changed beyond epsilon
     (playposition 1e-4, vu 0.01) or 1 s since last emit (keepalive).
   - Seek detection: |Δplayposition| > (2 × tick × rate)/duration while playing, or
     any jump while paused → immediate `deck.seek`.
   - `serverTimeMs` from a monotonic `QElapsedTimer` shared by the server.
5. **Late-joiner replay**: on client connect (post-`hello`), send current
   `deck.loaded` (or nothing if empty) + one `deck.tick` per deck, from a snapshot
   held by the worker.
6. **Generation discipline**: every deck message carries `generation`; snapshot
   updates and broadcasts happen on the worker thread only (single writer).
7. Tests: unit — generation monotonicity, tick-suppression logic, seek detector
   (feed synthetic position series). Integration — connect `QWebSocket` in test,
   fake deck events, assert event order: loaded → ticks → seek → unloaded.

## Acceptance criteria

- [ ] `websocat ws://127.0.0.1:24742/ws/v1` shows live ticks while a track plays.
- [ ] Load/eject on decks 1–4 produce correct loaded/unloaded events with
      incrementing generations; late-joining client gets replay immediately.
- [ ] Hotcue jump / needle drop → `deck.seek` within one tick interval.
- [ ] Position drift vs Mixxx UI, extrapolated per the client contract, stays
      within ±80 ms over 5 minutes of playback.
- [ ] 2-hour soak with one client: flat memory, no disconnects, Mixxx UI unaffected.
- [ ] Two simultaneous clients receive identical streams.

## Out of scope

Track metadata richness (T05), actions (T07), waveform (T09), auth (T08).

---

## Completion note (implemented, pending build verification)

WebSocket `/ws/v1` served on the same port via `Upgrade` handoff; client registry,
`hello`/`subscribe`/`ping`→`pong`, and on-connect replay of loaded decks in
`companionserver.cpp`. `deckstatepublisher.cpp` polls the control system on the
worker thread via the thread-safe `ControlObject::get()` (track_loaded, playposition,
duration, `rate_ratio` for effective rate, play, vu_meter) at `tick_interval_ms`,
change-gated with a 1 s keepalive and a jump detector that emits `deck.seek`.
Load/unload events originate from `BaseTrackPlayer` signals on the main thread
(`CompanionService`), which bumps a per-deck generation and forwards serialized JSON
to the server via queued signals; the server stamps generation + serverTimeMs.
Deferred: adversarial/soak verification and drift measurement (T11); auth-gating of
the WS upgrade (T08).
