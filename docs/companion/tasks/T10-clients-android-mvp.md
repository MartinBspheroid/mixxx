# T10 — Client Docs + Android MVP Plan

**Phase:** 4 · **Size:** M (docs) + separate-repo app work · **Blocked by:** T04–T07

## Objective

1. `docs/companion/CLIENT.md` — the complete client developer guide.
2. The Android app MVP plan (app lives in a **separate repo**, e.g.
   `mixxx-companion-android`; this fork only carries its contract + plan).

## Part 1 — CLIENT.md contents (write against the live API, not from memory)

- Connect flow: discover host (manual IP v1), pair (T08 QR/code), open WS, `hello`,
  handle replay.
- Event handling: generation discipline, tick extrapolation math with worked
  example (`pos(t)` formula from `03-api-spec.md`), reconnect with backoff +
  re-fetch `/v1/decks` snapshot.
- Reference snippets: MXWF decoder in **TypeScript** and **Kotlin** (~30 lines
  each, tested against a golden blob committed as a fixture).
- Full endpoint walkthrough with curl transcripts captured from a real session.

## Part 2 — Android MVP plan (summary; expand in the app repo)

**Stack (decision D9): Kotlin + Jetpack Compose**, min SDK 26, OkHttp (WS + HTTP),
kotlinx.serialization, Room for local cache (search results, session notes later),
DataStore for pairing token. Tablet-first layouts, keep-screen-on flag in HUD.

**Screens (MVP = 3):**

1. **Live HUD** — per deck: title/artist (large), BPM/key, elapsed/remaining,
   playing state, warning banners ("ends in 30 s", "other deck empty").
   Playhead over waveform once T09 ships; plain progress bar until then.
2. **Library** — search box + BPM/key filter chips → `/v1/library/search`,
   infinite scroll via offset paging; tap row → detail sheet (metadata + actions).
3. **Queue/Actions** — long-press or detail-sheet actions: Load to Deck 1/2
   (+ optional play), Add to Auto DJ. Confirmation dialog on load-to-playing-deck
   (`force` flag).

**App architecture:** single `CompanionRepository` owning the WS connection
(foreground service while HUD visible), `StateFlow<DeckState>` per deck, UI renders
at frame rate with local extrapolation between ticks; all writes go through typed
action calls mirroring T07.

**Non-goals (v1):** scrolling zoomed waveform, any performance controls (EQ/FX/
scratch), offline library, Wear OS/widgets (later-features list lives in
`00-overview.md` Phase 5).

## Acceptance criteria

- [ ] CLIENT.md lets a developer with no C++ knowledge build a working client
      (validate: the TS snippet + a 50-line node script renders correct song info
      + positions).
- [ ] Golden MXWF fixture + both decoder snippets round-trip.
- [ ] Android repo bootstrapped with the plan above as its README; HUD screen
      showing live song info from a real Mixxx session is the app's own MVP gate.
