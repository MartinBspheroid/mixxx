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

## Authentication (T08; MVP on localhost may run with auth disabled)

- Loopback connections: allowed without token by default.
- Non-loopback: require `Authorization: Bearer <token>` (HTTP) or
  `?token=<token>` (WS upgrade). Tokens are per-device, issued via pairing.
- Unauthorized → `401 {"error":{"code":"unauthorized"}}`.

## Error shape (all endpoints)

```json
{ "error": { "code": "not_found", "message": "no track 1234" } }
```
Codes: `bad_request`, `unauthorized`, `not_found`, `analysis_pending`,
`action_not_allowed`, `internal`.

---

## HTTP endpoints

### Read-only (MVP)

| Endpoint | Returns |
|---|---|
| `GET /v1/status` | service/app info, see below |
| `GET /v1/decks` | array of DeckStateDto (current snapshot, all decks) |
| `GET /v1/decks/:deck` | DeckStateDto (deck is 1-based int) |
| `GET /v1/tracks/:id` | TrackDto |
| `GET /v1/library/search?q=&bpmMin=&bpmMax=&key=&limit=&offset=` | SearchResult |
| `GET /v1/tracks/:id/waveform/summary` | binary MXWF blob (T09) |
| `GET /v1/tracks/:id/cues` | cue/loop markers (T09) |
| `GET /v1/playlists`, `/v1/playlists/:id/tracks` | Phase 3 |
| `GET /v1/crates`, `/v1/crates/:id/tracks` | Phase 3 |

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
  "libraryReady": true,
  "uptimeMs": 123456,
  "clients": 1,
  "auth": "open-loopback"
}
```

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

`deck.tick` — 10–20 Hz while state changes, ≥1 Hz keepalive per loaded deck:

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
  "serverTimeMs": 182934701
}
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
