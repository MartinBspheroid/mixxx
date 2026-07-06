# 02 — CompanionService Architecture

## Module layout

All new code lives in `src/companion/` (one new top-level module, mirroring how
`src/broadcast/` and `src/controllers/` are organized):

```
src/companion/
  companionservice.h/.cpp       # facade; owns everything below; lives on main thread,
                                # spawns the worker thread; wired into CoreServices
  companionserver.h/.cpp        # QWebSocketServer + HTTP listener; runs on worker thread
  companionsettings.h/.cpp      # enabled flag, port, bind address, tokens (QSettings-backed
                                # via UserSettings); Preferences page comes later
  deckstatepublisher.h/.cpp     # ControlProxy subscriptions per deck; tick timer;
                                # emits DeckTick/DeckLoaded/DeckUnloaded DTOs
  trackserializer.h/.cpp        # TrackPointer -> QJsonObject (TrackDto), stable field names
  libraryquery.h/.cpp           # search request marshalling to main thread; paginated results
  remoteactions.h/.cpp          # whitelisted action dispatch (load, play, pause, ...)
  waveformexporter.h/.cpp       # (T09) summary waveform -> MXWF binary blob
  pairingmanager.h/.cpp         # (T08) token issuance/validation, QR payload
  companiondtos.h               # plain structs for events; single place for schema
```

## Threading model

Three threads matter:

1. **Main thread** — Mixxx UI + library. `CompanionService` (the facade) lives here.
   All DAO/library access happens here. `BaseTrackPlayer` signals are emitted here.
2. **Companion worker thread** — a `QThread` with an event loop started by
   `CompanionService`. `CompanionServer`, all socket objects, all `ControlProxy`
   instances, and the tick timer live here. Rationale: serialization/socket I/O never
   competes with the GUI, and ControlProxy's create/delete-on-same-thread rule
   (issue #7773) is satisfied by construction.
3. **Audio/engine threads** — we never touch them. Control reads go through the
   lock-free control system; signals from the engine arrive queued.

Data flow rules:

- Main → worker: `Qt::QueuedConnection` signals carrying **copied DTO structs**
  (declare with `Q_DECLARE_METATYPE` + `qRegisterMetaType`). Never pass
  `TrackPointer` across; `TrackSerializer` runs on the main thread in the
  `newTrackLoaded` slot and sends the finished `QJsonObject`/struct.
- Worker → main (remote actions, library queries): queued signal into
  `CompanionService`, which calls `PlayerManager` slots / library code on the main
  thread, then replies with a queued signal back (request-id correlated).
- Control *reads* (`ControlObject::get`) are thread-safe anywhere; control
  *subscriptions* (ControlProxy) stay on the worker thread.
- Library search may alternatively use a dedicated read-only `QSqlDatabase`
  connection cloned for the worker thread (SQLite allows multiple readers). Start
  with main-thread marshalling (simpler, uses `SearchQueryParser` naturally);
  switch to the read-only clone only if profiling shows main-thread jank.

## Hard rules (non-negotiable, from the original plan — enforce in review)

1. **Never block the audio engine.** No allocation, locking, or I/O on engine
   threads. The companion service only observes queued signals and thread-safe
   control reads.
2. **Respect thread boundaries.** DAOs/TrackCollection: main thread only.
   ControlProxy: worker thread only, created there, deleted there.
3. **Read-only first.** Write surface is the explicit T07 whitelist. No delete
   track, no metadata edit, no file operations — ever, in v1.
4. **LAN requires pairing.** Default bind `127.0.0.1`. `0.0.0.0` bind requires the
   settings toggle *and* token auth (T08).
5. **Client is non-critical.** Service failure or disconnect must never affect
   playback. All companion code paths are wrapped so exceptions/errors degrade to
   "service stops", never "Mixxx crashes".

## Lifecycle

```
CoreServices::initialize()
  ... PlayerManager, TrackCollectionManager, Library constructed ...
  m_pCompanionService = std::make_shared<CompanionService>(
        pSettingsManager->settings(), m_pPlayerManager.get(),
        m_pTrackCollectionManager.get(), m_pLibrary.get());
  m_pCompanionService->start();   // no-op if disabled in settings

CoreServices::finalize()
  m_pCompanionService->stop();    // close sockets, quit worker thread, wait()
  m_pCompanionService.reset();    // BEFORE Library/PlayerManager teardown
  ... existing teardown ...
```

`start()` reads `CompanionSettings`; if disabled, it does nothing (zero runtime
cost — this keeps the fork trivially rebasable and upstream-friendly). If enabled,
it starts the worker thread, constructs `CompanionServer` on it, connects deck
signals, and begins ticking.

## Event pipeline (song info — the primary target)

```
BaseTrackPlayer::newTrackLoaded(TrackPointer)          [main thread]
  → CompanionService slot: serialize Track -> TrackDto, ++generation[deck]
  → queued signal to worker
  → CompanionServer broadcasts {"type":"deck.loaded", ...} to /ws/v1 clients

QTimer (worker thread, 100 ms default; 50–200 ms configurable)
  → DeckStatePublisher reads ControlObject::get() for each active deck
    (playposition, duration, rate, play, vu_meter)
  → broadcasts {"type":"deck.tick", ...} — only for decks whose values changed
    beyond epsilon, or at a 1 Hz keepalive floor

ControlProxy playposition (worker thread) large-jump detection
  → immediate {"type":"deck.seek", ...} out-of-band correction
```

Generation counter: incremented on every load/unload per deck; every deck message
carries it; clients drop messages whose generation ≠ latest for that deck.

## Error handling & observability

- Structured logging under a new category `mixxx.companion`
  (`qCDebug`/`qCWarning`), consistent with existing logging.
- `GET /v1/status` includes uptime, connected client count, and event counters —
  the first debugging tool for the phone app.
- Malformed client input → 400 with JSON error body; never assert/crash on input.
- Server errors are contained: a handler failure closes that connection at worst.

## Configuration (v1: config keys; Preferences UI later)

`UserSettings` group `[CompanionAPI]`:

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | master switch |
| `port` | `24742` | TCP port for HTTP + WS |
| `allow_lan` | `false` | bind 0.0.0.0 instead of 127.0.0.1 (requires pairing) |
| `tick_interval_ms` | `100` | deck tick period (50–200 clamped) |
| `expose_file_paths` | `false` | include `location` in TrackDto (paired clients only) |

A Preferences page (`src/preferences/dialog/`) is a stretch item inside T08 —
config keys are sufficient for the whole MVP.
