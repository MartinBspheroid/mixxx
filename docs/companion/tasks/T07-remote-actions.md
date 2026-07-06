# T07 — Remote Actions: Call Mixxx Functions from the Phone (PRIMARY TARGET)

**Phase:** 2 · **Size:** M · **Blocked by:** T05 (track resolution), T04 (state
reflection)

## Objective

The whitelisted `POST` actions of `03-api-spec.md`: load track to deck, play,
pause, cue, sync, seek, Auto DJ queue append. This completes the "call functions
from the phone" goal.

## Safety model (read first)

- **Whitelist, not gateway.** Each action is an explicit handler. There is no
  generic "set any ControlObject" endpoint in v1 — that would make the phone a
  performance surface and a security hazard.
- Actions that could wreck a live set get guards: `seek` refused while `playing`
  unless `"force": true`; `load` refused on a playing deck unless `"force": true`
  (mirrors Mixxx's own "Load track to playing deck" warning; check the user
  setting `[Controls],AllowTrackLoadToPlayingDeck` and honor it).

## Steps

1. `src/companion/remoteactions.h/.cpp`: action registry
   `{name → validator + main-thread executor}`. HTTP handler parses/validates on
   the worker, executes via queued call on the main thread, replies after
   execution (or after enqueue for fire-and-forget controls — document per action).
2. Implementations:
   - **load**: resolve `trackId` → `TrackPointer` via TrackCollectionManager (main
     thread); get location; call `PlayerManager::slotLoadLocationToPlayer(location,
     groupForDeck(deck-1), play)` — or `slotLoadTrackToPlayer` handling the
     `__STEM__` signature split (`01-codebase-map.md` §1 caveat). 404 unknown
     track, 400 bad deck index (validate against `numberOfDecks()`).
   - **play/pause**: `ControlObject::set(ConfigKey(group, "play"), 1.0/0.0)`.
   - **cue**: set `cue_default` 1.0 then 0.0 (press/release semantics — verify
     against how controller mappings do it, grep `cue_default` in `res/controllers`).
   - **sync**: `beatsync` momentary set.
   - **seek**: set `playposition` (0..1 validated).
   - **autodj/queue**: `PlaylistDAO` Auto DJ playlist append on main thread (find
     the Auto DJ playlist id via `PlaylistDAO::getPlaylistIdFromName` /
     `kAutoDJPlaylistName` — grep for how AutoDJFeature does it).
3. Every successful action → the resulting state change flows out via T04 events
   (verify no extra push needed; ticks/loaded events cover it).
4. Unknown action path → `action_not_allowed` (403), distinct from 404, so clients
   can feature-detect.
5. Tests: validator unit tests (bad deck, bad body, force flags); integration —
   load a fixture track to deck 1 via HTTP, assert `deck.loaded` arrives on WS and
   `getLoadedTrack()` matches; play/pause toggles `play` control; seek-while-playing
   without force → 400.

## Acceptance criteria

- [ ] From a phone browser/curl on the LAN (with T08, or loopback before it):
      search (T06) → pick id → `POST /v1/decks/1/load` → track loads in Mixxx and
      `deck.loaded` event arrives.
- [ ] Play/pause/cue/sync/seek work and are observable in both Mixxx UI and WS events.
- [ ] Guards: seek/load on playing deck refused without `force`.
- [ ] No action can run before the library is ready (`libraryReady:false` → 409-style
      `bad_request` with clear message).

## Out of scope

Volume/EQ/FX/crossfader (deliberately excluded), hotcue triggering (Phase 3
decision), generic control endpoint (never in v1).
