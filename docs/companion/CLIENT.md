# Companion API — Client Developer Guide

Everything you need to build a client (phone app, browser dashboard, ESP32, …)
against the Mixxx Companion API, without reading any C++. For the internal design
see the other docs in this folder; this file is the outward-facing contract.

The reference client is the Android HUD app (Kotlin + Compose) in the sibling
`android-app-mixxx/` repo — a working implementation of everything below.

## Overview

- One TCP port (default **24742**) serves both HTTP and WebSocket.
- **HTTP** for request/response: status, library search, track detail, cues,
  waveform, deck snapshots, and actions.
- **WebSocket** `/ws/v1` for the live event stream: deck load/tick/seek/unload.
- All JSON is UTF-8. Timestamps are server monotonic milliseconds (`serverTimeMs`)
  unless suffixed `Iso`.
- Versioned under `/v1`. Clients must ignore unknown JSON fields.

## 1. Connect & authenticate

### Loopback (same machine) — no auth

A client on `127.0.0.1` is trusted and needs no token. Great for local dashboards.

```
GET http://127.0.0.1:24742/v1/status
```

### LAN (phone over Wi-Fi) — the 6-digit session code

Non-loopback clients authenticate with the **session pairing code**: a 6-digit
code Mixxx generates at startup, prints prominently in its log, and serves via
`GET /v1/pair/code` (loopback-only, for a local helper/overlay to display).
It is valid for the whole Mixxx session with full control scope — the connect
flow is always simply: *read the code off Mixxx, type it on the phone*.

The code belongs to the Mixxx session, not to the server: it exists from startup
whether or not the API is enabled, so **Preferences → Companion API always shows
it**, and it survives enabling the API or changing the port (both restart the
server) so a phone you already paired keeps working. Only quitting Mixxx or
clicking **Regenerate** changes it.

That page also shows the **address to open on the phone** (`<ip>:<port>`), so
there is no need to go hunting for the machine's IP. LAN addresses are preferred
over VPN-overlay ones (Tailscale, 100.64/10) and both over routable addresses; a
multi-homed machine lists the alternatives, since only the user knows which
network the phone is on. With LAN off the server binds loopback, and the page
says so rather than offering an address no phone can reach.

Pass it on every request:

```
GET http://<laptop>:24742/v1/status?code=481920        # query param, or:
Authorization: Bearer 481920                            # header
ws://<laptop>:24742/ws/v1?code=481920                   # WebSocket upgrade
```

Wrong/missing code → `401 unauthorized`. A new code is generated each time
Mixxx starts, so the phone should re-prompt on 401 after a reconnect.

(Long-lived per-device tokens still exist underneath — `POST /v1/pair` +
`/v1/pair/claim` — for clients that want to skip code entry across sessions;
read-only tokens get `403` on actions. Optional; the code is the primary flow.)

> **Transport note:** v1 is plain HTTP — tokens are sniffable on open Wi-Fi. Use a
> trusted LAN or a personal hotspot. TLS/wss is a planned future addition.

## 2. Errors

Every error has the shape:
```json
{ "error": { "code": "not_found", "message": "optional detail" } }
```
Codes: `bad_request` (400), `unauthorized` (401), `action_not_allowed` (403),
`not_found` (404), `too_many_attempts` (429), `analysis_pending` (202),
`internal` (500).

## 3. HTTP endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/v1/status` | service/app info (below) |
| GET | `/v1/decks` | array of deck snapshots |
| GET | `/v1/decks/:deck` | one deck snapshot (1-based) |
| GET | `/v1/tracks/:id` | TrackDto |
| GET | `/v1/tracks/:id/cues` | cue markers (for a deck, prefer the pushed `deck.cues`) |
| GET | `/v1/tracks/:id/waveform/summary` | binary MXWF blob (§6) |
| GET | `/v1/library/search?q=&bpmMin=&bpmMax=&key=&limit=&offset=` | SearchResult (rows carry `playedTonight`) |
| GET | `/v1/decks/:deck/suggestions?limit=&bpmWindow=` | "what can I play next" — ranked compatible tracks with `score` + `reasons` |
| POST | `/v1/decks/:deck/load` `{trackId,play}` | load track to deck (202) |
| POST | `/v1/decks/:deck/play` \| `pause` \| `cue` \| `sync` | transport |
| POST | `/v1/decks/:deck/seek` `{position}` | seek 0..1 (refused while playing unless `force`) |
| POST | `/v1/autodj/queue` `{trackId}` | append to Auto DJ (202) |
| POST | `/v1/pair` | open pairing window (loopback only) |
| POST | `/v1/pair/claim` `{code,deviceName,readOnly}` | claim a token |
| GET/DELETE | `/v1/pair/devices[/:id]` | manage paired devices (loopback only) |

`GET /v1/status`:
```json
{ "app":"Mixxx", "version":"2.6.0-beta", "apiVersion":1, "numDecks":4,
  "libraryReady":true, "uptimeMs":52502, "clients":1,
  "auth":"open-loopback", "pairedDevices":false }
```

Actions that change deck state are also reflected as WebSocket events, so after
POSTing you don't need to poll — just watch the stream.

## 4. WebSocket `/ws/v1`

The server pushes events; the client may send a few messages.

### Client → server
```json
{ "type":"hello", "clientName":"android-hud", "protocol":1 }
{ "type":"subscribe", "topics":["decks"] }
{ "type":"ping", "t":12345 }        // server replies {"type":"pong","t":12345}
```

### Server → client
`deck.loaded` (on load, and replayed to each client on connect):
```json
{ "type":"deck.loaded", "deck":1, "generation":42,
  "track":{ "id":1234, "title":"Glue", "artist":"Bicep", "bpm":129.98,
            "key":"8A", "durationSeconds":269.3, "rating":4, "color":"#3F51B5" },
  "waveform":{ "summaryUrl":"/v1/tracks/1234/waveform/summary" },
  "serverTimeMs":182934701 }
```
`deck.beatgrid` (after `deck.loaded`, on every grid change, and replayed on
connect). `beats` is in seconds from the track start, stops at the track end, and
is empty until the track is analyzed — so draw ticks from this and expect a
second event once analysis lands, instead of polling the HTTP endpoint:
```json
{ "type":"deck.beatgrid", "deck":1, "generation":42, "bpm":129.98,
  "constantTempo":true, "beats":[0.021, 0.483, 0.945], "serverTimeMs":182934701 }
```
`deck.cues` (after `deck.loaded`, on every cue change, and replayed on connect).
`positionSeconds` is seconds from the track start; `lengthSeconds` appears for
loop/intro/outro; `index` is the hotcue number and is absent for non-hotcues.
Cue edits during a set — setting, clearing, moving a hotcue — re-fire this, so
render straight from it and never cache it against a track:
```json
{ "type":"deck.cues", "deck":1, "generation":42, "serverTimeMs":182934701,
  "cues":[ { "type":"hotcue", "index":0, "positionSeconds":32.145,
             "label":"drop", "color":"#3f51b5" },
           { "type":"loop", "positionSeconds":64.0, "lengthSeconds":8.0,
             "color":"#00b400" } ] }
```
`deck.tick` (10–20 Hz while state changes, ≥1 Hz keepalive):
```json
{ "type":"deck.tick", "deck":1, "generation":42, "playposition":0.437,
  "positionSeconds":117.7, "durationSeconds":269.3, "rate":1.0,
  "playing":true, "vu":0.74, "serverTimeMs":182934701 }
```
`deck.seek` (immediate on jumps): `{ "type":"deck.seek", "deck":1, "generation":42, "playposition":0.61, "serverTimeMs":… }`
`deck.unloaded`: `{ "type":"deck.unloaded", "deck":1, "generation":43 }`

### Rendering contract (do this)
- **Generation discipline:** every deck message has a `generation`. Track the
  latest per deck and **drop any message with an older generation** — otherwise a
  late packet paints the wrong track.
- **Local extrapolation:** don't jump the playhead on each tick. Between ticks,
  advance it yourself at display refresh rate:
  ```
  pos(t) = playposition + (t - serverTimeMs)/1000 * rate / durationSeconds   // while playing
  ```
  Correct to the authoritative value on each tick. Target tolerance is ±30–80 ms;
  this is an orientation HUD, not a scratch display.
- **Reconnect:** on socket drop, reconnect with backoff and re-fetch `/v1/decks`
  for a fresh snapshot; the server also replays `deck.loaded` on connect.

## 4b. Deck layout & the library browse HUD

**Deck layout:** the first replay message on connect is `decks.config`
(`{"numDecks":4,"visibleDecks":2}`) — render `visibleDecks` decks. It re-fires
if the user toggles the skin's 4-deck view.

**Library HUD:** the phone mirrors what the user is browsing in Mixxx — always
just the current folder and the cursor, exactly like a hardware controller
display:

- `library.view` — the user switched views (crate/playlist/folder/search):
  `{viewKey, search?, rowCount}`. Reset the list.
- `library.cursor` — the highlighted row settled (~100 ms debounce):
  `{viewKey, row, rowCount, track:{...}, window:{start, rows:[{row,id,title,artist,bpm,key}]}}`.
  Render `window.rows` as the visible slice, highlight `row`, show
  `row+1 / rowCount` as the position indicator. `row: -1` = selection cleared.
- Latest view + cursor are replayed on connect.

Navigation commands (mirror a browse knob — the resulting `library.cursor`
events are your confirmation):

```
POST /v1/library/move   {"delta": 1}    # cursor down/up N rows
POST /v1/library/scroll {"delta": 1}    # page down/up
POST /v1/library/focus  {"delta": 1}    # sidebar <-> track list
POST /v1/library/goto   {}              # enter/activate highlighted item
POST /v1/decks/1/loadSelected {"play": true}   # load cursor track & play
```

Suggested phone UX: a scroll wheel/list fling maps to `move` deltas; tapping a
visible row sends `move` with the row delta then `loadSelected` (or use
`/v1/decks/:deck/load {trackId}` directly since window rows carry ids).

## 4c. Beat grid (real beat markers)

For a track on a deck, prefer the pushed `deck.beatgrid` event (above): it
arrives unprompted and re-fires when the grid changes. This endpoint is for
grids you want off-deck (a library preview, say).

`GET /v1/tracks/:id/beatgrid` → `{trackId, bpm, constantTempo, beats:[seconds...]}`
(≤4096 beats; `truncated:true` if clipped). Use the `beats` array to draw exact
beat/bar ticks on the waveform (bars = every 4th beat) instead of extrapolating
from BPM — this handles variable-tempo tracks and off-grid first beats correctly.
Fetch once per loaded track (positions are in seconds; convert with
`durationSeconds` / `playposition`).

## 5. Search

`q` uses Mixxx's native search grammar (same as the desktop search box):
`bpm:120-128`, `key:8A`, `artist:bicep`, quoted `"phrases"`, `-exclusions`.
`bpmMin/bpmMax/key` are convenience filters ANDed on. `limit` defaults 50 (max
200); `total` is capped at 1000 (means "≥1000").

```json
{ "query":"bicep", "total":3, "offset":0, "limit":50,
  "tracks":[ { "id":1234, "title":"Glue", "artist":"Bicep", "album":"Bicep",
               "bpm":129.98, "key":"8A", "durationSeconds":269.3, "rating":4,
               "lastPlayedAtIso":"2026-07-01T22:14:03" } ] }
```

## 6. Waveform blob — `MXWF v1`

`GET /v1/tracks/:id/waveform/summary` returns `application/octet-stream`, or
`202 {"error":{"code":"analysis_pending"}}` if the track isn't analyzed yet
(retry after the next `deck.loaded` or a short backoff).

All integers little-endian. 32-byte header:

| Offset | Type | Field |
|---|---|---|
| 0 | char[4] | `"MXWF"` |
| 4 | u16 | version (1) |
| 6 | u16 | flags (0 = mono-mixed) |
| 8 | u32 | trackId |
| 12 | f32 | durationSeconds |
| 16 | u32 | sampleCount (visual frames) |
| 20 | u8 | channels (1) |
| 21 | u8 | bands (4) |
| 22 | u16 + u64 | reserved (0) |

Payload: `sampleCount × 4` bytes; per frame the bands `[all, low, mid, high]`,
each `uint8`. L/R are max-mixed into mono. Render `all` as the body and colour by
`low/mid/high` (Serato-style) if desired.

### Reference decoders

TypeScript:
```ts
type WaveBands = { all: Uint8Array; low: Uint8Array; mid: Uint8Array; high: Uint8Array };
function decodeMxwf(buf: ArrayBuffer): { trackId: number; durationSeconds: number; bands: WaveBands } {
  const dv = new DataView(buf);
  if (String.fromCharCode(dv.getUint8(0),dv.getUint8(1),dv.getUint8(2),dv.getUint8(3)) !== "MXWF")
    throw new Error("bad magic");
  const trackId = dv.getUint32(8, true);
  const durationSeconds = dv.getFloat32(12, true);
  const n = dv.getUint32(16, true);
  const all = new Uint8Array(n), low = new Uint8Array(n), mid = new Uint8Array(n), high = new Uint8Array(n);
  let o = 32;
  for (let i = 0; i < n; i++) { all[i]=dv.getUint8(o++); low[i]=dv.getUint8(o++); mid[i]=dv.getUint8(o++); high[i]=dv.getUint8(o++); }
  return { trackId, durationSeconds, bands: { all, low, mid, high } };
}
```

Kotlin (matches `android-app-mixxx/.../data/MxwfDecoder.kt`):
```kotlin
fun decodeMxwf(bytes: ByteArray): WaveBands {
    val bb = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
    require(bb.get()=='M'.code.toByte() && bb.get()=='X'.code.toByte() &&
            bb.get()=='W'.code.toByte() && bb.get()=='F'.code.toByte()) { "bad magic" }
    bb.short; bb.short                 // version, flags
    bb.int                             // trackId
    bb.float                           // durationSeconds
    val n = bb.int
    bb.get(); bb.get(); bb.short; bb.long   // channels, bands, reserved
    val all=FloatArray(n); val low=FloatArray(n); val mid=FloatArray(n); val high=FloatArray(n)
    for (i in 0 until n) {
        all[i]=(bb.get().toInt() and 0xFF)/255f; low[i]=(bb.get().toInt() and 0xFF)/255f
        mid[i]=(bb.get().toInt() and 0xFF)/255f; high[i]=(bb.get().toInt() and 0xFF)/255f
    }
    return WaveBands(all, low, mid, high)
}
```

### Performance note (learned the hard way)
Summary waveforms are ~3840 samples. Redrawing all of them every frame for 4
decks caused ~1.8 s/frame jank in the reference app. **Downsample** the overview
to your pixel width (cap ~480 columns) and the scrolling view (~1920) with a
max-pooling reduction before drawing.

## 7. Minimal client flow

```
1. Discover host (manual IP for now). Pair if on LAN; store token.
2. GET /v1/status  (sanity + numDecks).
3. Open /ws/v1 (with ?token= on LAN). Send hello.
4. On deck.loaded: show metadata; GET the waveform summary (retry on 202).
5. On deck.tick: extrapolate the playhead locally at 60fps; correct on each tick.
6. Library screen: GET /v1/library/search; tap a row → POST /v1/decks/:n/load.
7. Never trust generation-stale packets.
```
