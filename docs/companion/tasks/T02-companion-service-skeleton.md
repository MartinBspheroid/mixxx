# T02 — CompanionService Skeleton, Worker Thread, CoreServices Wiring

**Phase:** 1 · **Size:** M · **Blocks:** T03–T09

## Objective

Create the `CompanionService` facade with settings, worker thread lifecycle, and
`CoreServices` integration — no endpoints yet. After this task, enabling
`[CompanionAPI] enabled` makes Mixxx start/stop a (still silent) service cleanly.

## Prerequisites

T01 merged. Read `02-architecture.md` fully (threading model + lifecycle are the
whole point of this task) and `01-codebase-map.md` §8.

## Files to create

- `src/companion/companionservice.h/.cpp` — facade, main thread. Ctor takes
  `UserSettingsPointer`, `PlayerManager*`, `TrackCollectionManager*`, `Library*`.
  `start()` / `stop()` per the lifecycle in `02-architecture.md`.
- `src/companion/companionsettings.h/.cpp` — typed accessors over the
  `[CompanionAPI]` config group (keys + defaults from `02-architecture.md` §Configuration).
- `src/companion/companionserver.h/.cpp` — worker-thread object; this task only:
  constructed on the worker thread, logs startup, owns nothing yet.
- `src/companion/companiondtos.h` — empty-ish placeholder with the DTO structs
  from `03-api-spec.md` (TrackDto, DeckStateDto as plain structs) +
  `Q_DECLARE_METATYPE`.
- Remove `src/companion/companionsmoke.cpp` from T01.

## Files to modify

- `src/coreservices.h/.cpp` — add `#ifdef __COMPANION__` member
  `std::shared_ptr<CompanionService> m_pCompanionService;` construct in
  `initialize()` **after** PlayerManager/TrackCollectionManager/Library are built;
  `stop()` + `reset()` **early** in `finalize()`. Locate construction points by
  grepping for `m_pPlayerManager = ` and the first line of `finalize()` — do not
  trust line numbers.
- `CMakeLists.txt` — add the new sources to the guarded block.

## Steps

1. Implement `CompanionSettings` (pure read of `UserSettings`; no UI).
2. Implement `CompanionService::start()`: if disabled → log
   (`qCInfo(kCompanionLog)`) and return. Else create `QThread` named
   `"CompanionAPI"`, create `CompanionServer` via the
   moveToThread-then-construct-members pattern (or construct in a lambda invoked on
   the thread), connect `QThread::finished` for deletion.
3. Implement `stop()`: queued invoke of server shutdown, `thread->quit()`,
   `thread->wait(5000)`, log if timeout. Must be idempotent and safe if `start()`
   never ran.
4. Add logging category `mixxx.companion` (follow an existing
   `Q_LOGGING_CATEGORY` usage in the tree for placement conventions).
5. Wire into `CoreServices` (guarded by `__COMPANION__`).
6. Test: `src/test/companion/companionservicetest.cpp` — construct/start/stop 100×
   with enabled=false and (port 0) enabled=true; no crash, no leak (run with ASan
   if the local build supports it).

## Acceptance criteria

- [ ] Mixxx starts and quits cleanly with the service enabled and disabled.
- [ ] Worker thread visible in a debugger as "CompanionAPI" when enabled.
- [ ] Clean shutdown: no "QThread destroyed while running" warnings, ever.
- [ ] Start/stop stress test passes.
- [ ] `COMPANION_API=OFF` still builds (CoreServices guards correct).

## Out of scope

Sockets, endpoints, ControlProxy, deck signals (T03/T04).
