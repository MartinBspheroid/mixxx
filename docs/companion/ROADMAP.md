# Companion API — Forward Roadmap (post-v1)

v1 is complete and shipped: deck state + song info, library search, remote
actions (load/transport/seek/autodj/library-nav), waveform (MXWF) + beatgrid +
cues + cover art, pairing (6-digit session code), deck-config awareness, the
library-browse HUD, and the security/test hardening from the audit.

This is what's *missing* and worth building next, grouped by value. Each phase is
sized so a single agent can take it. Ordering is a suggestion, not a dependency
chain — A and B are the highest-leverage.

Guiding constraints (from the project): **HUD-first** (phone = eyes + brain, not a
performance surface); Qt-only, no platform `#ifdef`s; base on 2.6; ship
incrementally with tests.

---

## Phase A — Library intelligence: "what can I play next" ★ the killer feature

This was called out as *the* killer feature in the original plan and never built.
Everything it needs now exists server-side (search, key, bpm, `/v1/history`).

- `GET /v1/decks/:deck/suggestions` — given what's on a deck, return compatible
  next tracks scored by:
  - **harmonic key** (Camelot adjacency: same, ±1, relative major/minor),
  - **tempo** (±N BPM, honoring the current rate),
  - **energy** (rating/BPM heuristic, or a stored energy tag — see Phase C),
  - **freshness** (exclude tracks played this session via the history log),
  - **length** (avoid too-long tracks near set end).
- Each result carries **reason tags** the phone shows verbatim: `"perfect key"`,
  `"+1 energy"`, `"safe blend"`, `"already played"`, `"big BPM jump"`.
- `playedTonight` boolean on search + suggestion rows (join against the current
  set-log playlist).
- Server-side, main-thread SQL like `runLibrarySearch`; a `harmonic.h` helper for
  Camelot math. Fully unit-testable (key adjacency + scoring are pure functions).

**Why first:** turns the app from a mirror into a tool; pure server work; testable.

## Phase B — Stems ★ the 2.6 differentiator

The fork is on 2.6 specifically for stems — but the API exposes none of it.

- `deck.tick` (or a `deck.stems` event): per-stem `{label, volume, muted}` for the
  up-to-4 stems (`[ChannelN_StemM]` controls; `groupForDeckStem`).
- `POST /v1/decks/:deck/stems/:stem/{mute,volume}` — stem mute/solo/level
  (HUD-safe: these are mix decisions the phone can own).
- Stem-aware waveform: MXWF already reserves a stems flag/bit; add an
  `?stems=1` variant that emits the per-stem bands `Waveform::hasStem()` exposes.

**Why:** unique to this fork's 2.6 base; visually and functionally distinctive.

## Phase C — Prep & session workflow

- **Rate / tag from the phone**: `POST /v1/tracks/:id/rating {rating}` and simple
  energy tags (write path; safe, non-performance).
- **Prep queue / shortlist**: a server-held ordered list the phone edits (or lean
  on Auto DJ). `GET/POST/DELETE /v1/prep`.
- **Auto DJ control**: `GET /v1/autodj` (enabled, queue), `POST /v1/autodj/{enable,
  disable,skip}` — we already have queue-append.
- **Session summary**: `GET /v1/history/current` metadata (start time, track count)
  + an export (M3U/CSV) of what was played.

## Phase D — HUD-safe navigation control

Not "performance" (no EQ/crossfader/scratch) — these are cueing/navigation the
phone can safely drive, like a browse controller:

- hotcue **jump** (`POST /v1/decks/:deck/hotcue/:n/goto`), beatjump, loop
  set/halve/double/reloop, quantize toggle, keylock toggle.
- **Read-only FX status** in `deck.tick`: active FX unit + effect name + wet/dry,
  so the HUD can show "Echo out" without controlling it.

## Phase E — Waveform v2 & structure

- **High-res main waveform** (`/v1/tracks/:id/waveform/main`, 441 samples/s) for
  crisp zoomed scrolling — the app currently windows the ~3840-sample summary.
- **Phrase / structure markers** if/when analysis provides them (intro/outro
  already come via cues).

## Phase F — Reach, discovery, multi-user

- **TLS / wss** (`QSslServer`) for untrusted networks — documented as the one
  future security addition; keep the plain path for trusted LAN.
- **mDNS/Bonjour discovery** (currently skipped) — `_mixxx-companion._tcp`, so the
  phone finds Mixxx without typing an IP.
- **Party request mode**: guests browse a limited view and *request* a track; the
  DJ approves on their own device. Read-only token scope already supports the
  guest role.

## Phase G — Android app screens (separate `android-app-mixxx` repo)

Consume the above:

- **"What to play next"** screen (Phase A) — the flagship.
- **Prep / queue** and **session history** screens (Phase C).
- Real beat/bar ticks from the pushed `deck.beatgrid` event (drop the BPM-math
  fake). No polling: it lands after `deck.loaded`, on connect, and on every grid
  change, so ticks stay right when analysis finishes or the DJ nudges the grid.
- Stem strip on the deck HUD (Phase B).
- Later: home-screen widget, Wear OS glance, request-mode guest UI.

## Cross-cutting infra (not features, but overdue)

- **Integration tests** for the server (route dispatch + auth gating) — v1 has
  unit tests for the pure pieces (parser, MXWF, pairing, serializer) but no
  end-to-end route test.
- **T11 macOS build** + **T12 Raspberry Pi build recipe** (`BUILD-RASPI.md`) — the
  two platform passes that need real hardware.
- **Live-display verification** of the Preferences dialog and `library.cursor`
  streaming (headless can't drive them).

---

### Recommended first two

**A (library intelligence)** and **B (stems)**. A delivers the product's original
promise with pure, testable server work; B is the feature no other companion app
can match because it rides this fork's 2.6 base. Everything else is polish on a
already-solid v1.
