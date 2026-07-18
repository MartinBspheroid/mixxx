# 03 — Companion API Specification (v1)

Single TCP port (default **24742**) serving both HTTP and WebSocket
(`Upgrade: websocket` on `/ws/v1`). All JSON is UTF-8. All timestamps are server
monotonic milliseconds (`serverTimeMs`) unless suffixed `Iso`. This document is the
contract — clients and the service are both written against it, and every change
lands here in the same PR as the code.

## Versioning & compatibility

- Path-versioned: `/v1/...`, `/ws/v1`. Breaking change ⇒ `/v2`.
- Additive fields may appear at any time; clients must ignore unknown fields.
- `GET /v1/status` reports `apiVersion` (integer, currently `1`).
- **HTTP is one request per connection** (`Connection: close`); HTTP keep-alive
  is intentionally not implemented. All high-frequency state streams over the
  single long-lived `/ws/v1` WebSocket, so HTTP is only occasional
  request/response where connection churn on a LAN is negligible. Clients that
  need many small requests at once (e.g. a grid of covers) should use parallel
  connections.

## Authentication

- Loopback connections: allowed without a credential.
- Non-loopback (LAN): authenticate with the **6-digit session pairing code** —
  the primary, user-visible credential. Mixxx generates it at startup, prints it
  prominently in the log, and serves it to local helpers via
  `GET /v1/pair/code` (loopback-only). It is valid for the whole session with
  full control scope. Pass it as `?code=123456` (HTTP or WS upgrade) or
  `Authorization: Bearer 123456`. The code is tied to the Mixxx session rather
  than the server, so it is always displayed in Preferences → Companion API
  (even before the API is enabled) and is stable across a restart of the server;
  only quitting Mixxx or **Regenerate** rotates it.
- Long-lived per-device tokens (via `POST /v1/pair` + `/v1/pair/claim`) remain
  available underneath for clients that want to skip code entry, but the
  standard connect flow is always: read the code off Mixxx, type it on the
  phone.
- Unauthorized → `401 {"error":{"code":"unauthorized"}}`.

## Error shape (all endpoints)

```json
{ "error": { "code": "not_found", "message": "no track 1234" } }
```
Codes: `bad_request`, `unauthorized`, `not_found`, `analysis_pending`,
`action_not_allowed`, `too_many_attempts` (429 — per-IP auth throttle after
repeated failures; exponential lockout), `internal`.

CORS: all responses carry `Access-Control-Allow-Origin: *` and `OPTIONS`
preflights are answered, so browser dashboard clients work cross-origin.

---

## HTTP endpoints

### Read-only (MVP)

| Endpoint | Returns |
|---|---|
| `GET /v1/status` | service/app info, see below |
| `GET /v1/decks` | array of DeckStateDto (current snapshot, all decks) |
| `GET /v1/decks/:deck` | DeckStateDto (deck is 1-based int) |
| `GET /v1/tracks/:id` | TrackDto |
| `GET /v1/library/search?q=&bpmMin=&bpmMax=&key=&limit=&offset=` | SearchResult (fixed order: artist, title — may differ from the desktop's sort) |
| `GET /v1/tracks/:id/waveform/summary` | binary MXWF blob (T09) |
| `GET /v1/tracks/:id/cues` | cue/loop markers (T09). Decks also get this pushed as `deck.cues` — prefer that. |
| `GET /v1/tracks/:id/beatgrid` | `{trackId,bpm,constantTempo,beats:[seconds],truncated?}` (≤4096 beats, stops at track end). Decks also get this pushed as `deck.beatgrid` — prefer that. |
| `GET /v1/tracks/:id/cover` | cover art JPEG, ≤512px (404 if none) |
| `GET /v1/pair/code` | session pairing code (loopback only) |
| `POST /v1/pair`, `POST /v1/pair/claim` | pairing window / claim token (see Authentication) |
| `GET/DELETE /v1/pair/devices[/:id]` | manage long-lived token devices (loopback only) |
| `GET /v1/playlists` | `{playlists:[{id,name,trackCount}]}` (hidden system playlists excluded) |
| `GET /v1/playlists/:id/tracks` | `{id,name,tracks:[TrackSearchRow + position]}` in playlist order |
| `GET /v1/crates` | `{crates:[{id,name,trackCount}]}` |
| `GET /v1/crates/:id/tracks` | `{id,name,tracks:[TrackSearchRow]}` by artist/title |
| `GET /v1/history/current/tracks` | current session history: `{id,name,tracks:[... + position, playedAtIso]}` |
| `GET /v1/decks/:deck/suggestions?limit=&bpmWindow=` | "what can I play next" for the track on `deck` — see below |

### Actions (T07, POST, JSON body, whitelist only)

| Endpoint | Body | Effect |
|---|---|---|
| `POST /v1/decks/:deck/load` | `{"trackId": 1234, "play": false}` | load library track to deck |
| `POST /v1/decks/:deck/play` | `{}` | play |
| `POST /v1/decks/:deck/pause` | `{}` | pause |
| `POST /v1/decks/:deck/cue` | `{}` | cue_default behavior |
| `POST /v1/decks/:deck/sync` | `{}` | momentary beatsync |
| `POST /v1/decks/:deck/seek` | `{"position": 0.25}` | seek (0..1); refused while playing unless `"force":true` |
| `POST /v1/autodj/queue` | `{"trackId": 1234}` | append to Auto DJ queue |
| `POST /v1/library/move` | `{"delta": 1}` | move library cursor by N rows (browse knob) |
| `POST /v1/library/scroll` | `{"delta": 1}` | page up/down in the library |
| `POST /v1/library/focus` | `{"delta": 1}` | move focus between sidebar and track list |
| `POST /v1/library/goto` | `{}` | activate the highlighted item (enter folder) |
| `POST /v1/decks/:deck/loadSelected` | `{"play": false}` | load the highlighted library track to deck |

Actions return `200 {"ok":true}` or an error. Every action is also reflected as a
subsequent WS event (state change), so clients never need to poll after acting.
**Not in v1 by design:** volume/EQ/crossfader, effects, delete/edit metadata,
file operations, scratching — the phone is not a performance surface.

### `GET /v1/status` response

```json
{
  "app": "Mixxx",
  "version": "2.6-beta (companion fork)",
  "apiVersion": 1,
  "numDecks": 4,
  "visibleDecks": 2,
  "libraryReady": true,
  "uptimeMs": 123456,
  "clients": 1,
  "auth": "open-loopback",
  "pairedDevices": false
}
```

`clients` counts live WebSocket connections (including session-code clients).
`pairedDevices` refers only to long-lived *token* devices — a phone connected
via the session code intentionally does not appear there.

```
```

### `GET /v1/decks/:deck/suggestions` — "what can I play next"

Given the track currently on `deck`, returns compatible next tracks ranked best
first. Candidates are prefiltered to a `±bpmWindow` tempo band (default `8` BPM)
and to harmonically compatible or key-unknown tracks, then scored by the pure
[harmonic core](../../src/companion/harmonic.h): key relation (same / relative /
adjacent on the Camelot–Open-Key wheel), tempo closeness, energy direction
(BPM up/down), rating, and **session freshness** — a track already in the current
set-log is heavily penalized so a repeat never outranks a fresh option. The
`fromBpm` used is the deck's live, rate-adjusted BPM.

`limit` (1–100, default 20) caps results; `bpmWindow` (≤0 → default) widens or
narrows the tempo band. `404 not_found` if the deck is empty.

```json
{
  "deck": 1,
  "fromTrack": { "id": 1234, "title": "Glue", "artist": "Bicep",
                 "bpm": 129.98, "key": "8A" },
  "suggestions": [
    { "id": 5678, "artist": "Four Tet", "title": "Baby", "bpm": 130.0,
      "key": "8A", "durationSeconds": 312.0, "rating": 5, "score": 82.0,
      "reasons": ["perfect key", "tempo match"] },
    { "id": 9012, "artist": "Overmono", "title": "So U Kno", "bpm": 132.0,
      "key": "9A", "durationSeconds": 300.0, "playedTonight": true,
      "score": -62.0, "reasons": ["safe blend", "+1 energy", "already played"] }
  ]
}
```

`reasons` are short tags the client shows verbatim (`"perfect key"`,
`"energy switch"`, `"safe blend"`, `"key clash"`, `"tempo match"`,
`"slight stretch"`, `"big BPM jump"`, `"+1 energy"`, `"-1 energy"`,
`"already played"`). `playedTonight` (also added to `library/search` rows) marks
tracks already in the current set-log.

---

## WebSocket `/ws/v1`

Server pushes events; client may send a small set of messages. All frames are JSON
text except waveform blobs (HTTP-only in v1 — see D6/D8).

### Client → server

```json
{ "type": "hello", "clientName": "android-hud", "protocol": 1 }
{ "type": "subscribe", "topics": ["decks", "library"] }   // default: ["decks"]
{ "type": "ping", "t": 12345 }                            // server echoes "pong"
```

Topic routing (enforced): `library.*` events go only to clients subscribed to
`"library"`; everything else (`deck.*`, `decks.config`, `master.tick`) is the
`"decks"` topic. Subscribing to `"library"` late triggers an immediate replay
of the current `library.view` + `library.cursor`. Unknown topics are ignored;
an empty topic list resets to the default.

### Server → client events

`deck.loaded` — sent on track load and, for each deck, on client connect (replay of
current state so late joiners are instantly correct):

```json
{
  "type": "deck.loaded",
  "deck": 1,
  "generation": 42,
  "track": {
    "id": 1234,
    "title": "Glue",
    "artist": "Bicep",
    "album": "Bicep",
    "genre": "Electronic",
    "bpm": 129.98,
    "key": "8A",
    "durationSeconds": 269.3,
    "rating": 4,
    "playCount": 7,
    "color": "#3F51B5"
  },
  "waveform": { "summaryUrl": "/v1/tracks/1234/waveform/summary" },
  "serverTimeMs": 182934701
}
```

`deck.beatgrid` — the deck's beat grid, pushed right after `deck.loaded`, again
whenever the grid changes (analysis finishes, tap BPM, manual adjust), and on
client connect (replayed after that deck's `deck.loaded`). Same payload as
`GET /v1/tracks/:id/beatgrid` minus `trackId`, plus the deck framing — so a
client that renders beat ticks never has to poll the HTTP endpoint.

`beats` holds **seconds from the start of the track**, is always present (empty
when the track has no grid yet), stops at the end of the track, and is capped at
4096 entries — `truncated: true` says beats were dropped. Bursts of grid edits
are coalesced (~200 ms), so expect the last state, not every intermediate one.

Drop any event whose `generation` is not the deck's current one: it describes a
track that has already been replaced.

```json
{
  "type": "deck.beatgrid",
  "deck": 1,
  "generation": 42,
  "bpm": 129.98,
  "constantTempo": true,
  "beats": [0.021, 0.483, 0.945, 1.407],
  "serverTimeMs": 182934701
}
```

`deck.cues` — the deck's cue points, pushed on the same schedule as
`deck.beatgrid`: right after `deck.loaded`, again on **every** cue change (hotcue
set, cleared or moved, label or colour edited, intro/outro adjusted), and
replayed on client connect. Same payload as `GET /v1/tracks/:id/cues` minus
`trackId`, plus the deck framing.

`positionSeconds` is seconds from the start of the track — draw markers from it
rather than deriving anything from the waveform. `lengthSeconds` is present for
cues with an end (loop, intro, outro). `index` is the hotcue number and is absent
for cues that are not hotcues. `cues` is always present and may be empty. Bursts
are coalesced (~200 ms), so a "clear all hotcues" arrives as one event, not four.

The same `generation` rule applies: drop events for a generation that is no
longer the deck's.

```json
{
  "type": "deck.cues",
  "deck": 1,
  "generation": 42,
  "cues": [
    { "type": "maincue", "positionSeconds": 0.512, "color": "#ff8000" },
    { "type": "hotcue", "index": 0, "positionSeconds": 32.145,
      "label": "drop", "color": "#3f51b5" },
    { "type": "loop", "positionSeconds": 64.0, "lengthSeconds": 8.0,
      "color": "#00b400" }
  ],
  "serverTimeMs": 182934701
}
```

`deck.tick` — 10–20 Hz while state changes, ≥1 Hz keepalive per loaded deck.
`rate` is the playback **ratio** (1.0 = original tempo; ±6% pitch ⇒ 0.94–1.06).
`beatDistance` is the phase within the current beat (0..1) for phase meters.
`syncMode`: 0 = off, 1 = follower, 2 = leader. `loopStart`/`loopEnd` (present
only while `loopEnabled`) are normalized 0..1 like `playposition`, so clients
can draw the loop region directly on the waveform:

```json
{
  "type": "deck.tick",
  "deck": 1,
  "generation": 42,
  "playposition": 0.43712,
  "positionSeconds": 117.71,
  "durationSeconds": 269.3,
  "rate": 1.0,
  "playing": true,
  "vu": 0.74,
  "vuLeft": 0.71,
  "vuRight": 0.74,
  "beatDistance": 0.25,
  "syncMode": 0,
  "keylock": true,
  "loopEnabled": true,
  "loopStart": 0.41,
  "loopEnd": 0.45,
  "serverTimeMs": 182934701
}
```

`master.tick` — master-bus levels, change-gated at the tick cadence:

```json
{ "type": "master.tick", "vu": 0.8, "vuLeft": 0.78, "vuRight": 0.8,
  "serverTimeMs": 182934701 }
```

`deck.seek` — immediate, out-of-band on jumps (hotcue, needle drop, beatjump):

```json
{ "type": "deck.seek", "deck": 1, "generation": 42,
  "playposition": 0.6123, "serverTimeMs": 182991882 }
```

`deck.unloaded`:

```json
{ "type": "deck.unloaded", "deck": 1, "generation": 43 }
```

`decks.config` — deck layout; sent on connect (first replay message) and on
change, so the client renders 2 or 4 decks correctly. `numDecks` is the engine
deck count (`[App],num_decks`); `visibleDecks` is what the skin shows
(`[Skin],show_4decks` off → 2):

```json
{ "type": "decks.config", "numDecks": 4, "visibleDecks": 2,
  "serverTimeMs": 182934701 }
```

### Library browse HUD events

The phone mirrors the user's library navigation as a heads-up display: the
current view (folder/crate/playlist/search) and the cursor (highlighted row),
with a small window of rows around it. Sources inside Mixxx:
`Library::showTrackModel` (view switched), `Library::trackSelected` (cursor
settled, debounced ~100 ms by the track table). Latest view + cursor are
replayed to clients on connect.

`library.view` — the active view changed:

```json
{ "type": "library.view", "viewKey": "library:", "search": "bicep",
  "rowCount": 184, "serverTimeMs": 182934701 }
```

`viewKey` is Mixxx's stable model identifier for the view (`modelKey`), e.g.
the main library, a crate, a playlist, or a browse folder. `search` is present
when a search filter is active.

`library.cursor` — the highlighted track changed (`row: -1` = selection
cleared/multi-select). `window` carries the rows the phone should display, so
the HUD shows exactly the slice of the folder around the cursor:

```json
{ "type": "library.cursor", "viewKey": "library:", "row": 42, "rowCount": 184,
  "track": { "id": 1234, "title": "Glue", "artist": "Bicep", "bpm": 129.98,
             "key": "8A", "durationSeconds": 269.3 },
  "window": { "start": 38,
    "rows": [ { "row": 38, "id": 1201, "title": "…", "artist": "…",
                "bpm": 124.0, "key": "5A" } ] },
  "serverTimeMs": 182934701 }
```

`library.changed` — coarse invalidation, Phase 3:

```json
{ "type": "library.changed", "tracksAdded": [1239], "tracksChanged": [1234],
  "tracksRemoved": [] }
```

### Client rendering contract (documents the sync model)

- Client extrapolates: `pos(t) = playposition + (t - serverTimeMs)/1000 × rate / durationSeconds`
  while `playing`, corrected every tick; render at 60 FPS locally.
- Drop any deck message whose `generation` is less than the latest seen for that deck.
- Visual tolerance target: ±30–80 ms. This is an orientation HUD, not a scratch display.

---

## DTOs

### TrackDto

```ts
type TrackDto = {
  id: number
  title: string
  artist: string
  album?: string
  genre?: string
  comment?: string
  bpm?: number
  key?: string              // Lancelot notation from getKeyText()
  durationSeconds: number
  rating?: number           // 0..5
  playCount?: number
  color?: string            // "#RRGGBB"
  location?: string         // ONLY when expose_file_paths=true AND client is paired
}
```

### DeckStateDto (HTTP snapshot; superset of tick+loaded)

```ts
type DeckStateDto = {
  deck: number              // 1-based
  generation: number
  track?: TrackDto          // absent when empty
  playposition: number      // 0..1
  positionSeconds: number
  durationSeconds: number
  rate: number
  playing: boolean
  vu: number
}
```

### SearchResult

```ts
type SearchResult = {
  query: string
  total: number             // total matches (may be capped at 1000)
  offset: number
  limit: number             // default 50, max 200
  tracks: TrackSearchRow[]
}
type TrackSearchRow = {
  id: number
  title: string
  artist: string
  album?: string
  bpm?: number
  key?: string
  durationSeconds: number
  rating?: number
  lastPlayedAtIso?: string
}
```

Search semantics: `q` uses Mixxx's native search grammar via `SearchQueryParser`
(so `bpm:120-128`, `key:8A`, `artist:bicep`, quoted phrases, `-exclusions` all work
identically to the desktop search box). `bpmMin/bpmMax/key` are convenience
parameters ANDed onto `q`.

---

## Binary waveform format `MXWF v1` (T09)

`GET /v1/tracks/:id/waveform/summary` →
`Content-Type: application/octet-stream`. If analysis is missing:
`202 {"error":{"code":"analysis_pending"}}` — client retries after the next
`deck.loaded` or with backoff. **All multi-byte integers little-endian.**

Header (32 bytes):

| Offset | Type | Value |
|---|---|---|
| 0 | char[4] | magic `"MXWF"` |
| 4 | u16 | version = 1 |
| 6 | u16 | flags (bit0: stereo interleaved; 0 in v1 = mono-mixed) |
| 8 | u32 | trackId |
| 12 | f32 | durationSeconds |
| 16 | u32 | sampleCount (3840 for summary) |
| 20 | u8 | channels (1) |
| 21 | u8 | bands (4: all, low, mid, high) |
| 22 | u16 | reserved (0) |
| 24 | u64 | reserved (0) |

Payload: `sampleCount × bands` bytes, per-sample interleaved
`[all, low, mid, high]`, each `uint8`. v1 mixes L/R by max(). Size ≈ 15 KB per
track. Rationale: send data once per load; the client renders overview + playhead;
never stream rendered frames.

Source: `Waveform` from `Track::getWaveformSummary()`; per-sample accessors
`getAll/getLow/getMid/getHigh`. Stems bands are excluded from v1 (flags bit
reserved for a future stem-aware format).
