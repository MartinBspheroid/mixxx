# T05 — TrackSerializer: Loaded-Track Metadata DTO (PRIMARY TARGET)

**Phase:** 1 · **Size:** S–M · **Blocked by:** T02 (integrates with T04)

## Objective

`TrackSerializer` turning a `TrackPointer` into the `TrackDto` JSON of
`03-api-spec.md`, used by `deck.loaded` events and `GET /v1/tracks/:id` /
`GET /v1/decks[...]` snapshots.

## Steps

1. `src/companion/trackserializer.h/.cpp`, **main-thread-only** free function or
   static class: `QJsonObject serializeTrack(const TrackPointer&, const SerializeOptions&)`.
   Field mapping (verified getters, `01-codebase-map.md` §3):

   | JSON | Source |
   |---|---|
   | `id` | `track->getId().toInt()` (guard invalid id → omit endpoint-side) |
   | `title` / `artist` / `album` / `genre` / `comment` | corresponding getters; empty → omit |
   | `bpm` | `getBpm()`; ≤0 → omit |
   | `key` | `getKeyText()`; empty → omit |
   | `durationSeconds` | `getDuration()` |
   | `rating` | `getRating()`; 0 → omit |
   | `playCount` | `getTimesPlayed()` |
   | `color` | track color if set → `#RRGGBB` |
   | `location` | `getLocation()` **only if** `options.exposeFilePaths` |

2. Options struct wired from `CompanionSettings::exposeFilePaths()` (default false).
   This is a privacy boundary — full paths never leave the machine by default.
3. `GET /v1/tracks/:id`: HTTP handler (worker) → queued request to facade (main
   thread) → resolve via `TrackCollectionManager`
   (`internalCollection()->getTrackDAO()` lookup by `TrackId`) → serialize → queued
   response. Correlate by request id; 404 if unknown. Follow the request-marshalling
   helper pattern; if none exists yet, build a small
   `runOnMainThread(std::function<QJsonDocument()>)` utility in
   `companionservice` for reuse by T06/T07.
4. `GET /v1/decks` and `/v1/decks/:deck`: snapshot DTOs from the worker's deck
   state (T04) + serialized track.
5. Integrate with T04's `deck.loaded` (replace the title-only stub).
6. Tests: fixture Track → exact JSON (this pins the API contract); omission rules;
   `exposeFilePaths` on/off.

## Acceptance criteria

- [ ] `deck.loaded` carries full metadata for a real library track (verify bpm/key
      match Mixxx UI).
- [ ] `curl /v1/tracks/<id>` and `/v1/decks` return spec-shaped JSON; unknown id → 404.
- [ ] No `location` field unless the setting is on.
- [ ] Serializer never touches DAOs off the main thread (assert with
      `DEBUG_ASSERT(QThread::currentThread() == qApp->thread())`).

## Out of scope

Search (T06), artwork endpoints (Phase 3), cue markers (T09).

---

## Completion note (implemented, pending build verification)

`trackserializer.h/.cpp`: `serializeTrack(const TrackPointer&, bool exposeFilePaths)`
→ `TrackDto` JSON. Verified getters: getId (TrackId::toVariant().toInt()), getTitle,
getArtist, getAlbum, getGenre, getComment, getBpm, getKeyText, getDuration, getRating,
getTimesPlayed, getColor (`mixxx::RgbColor::toQString`), getLocation (gated).
Omission rules applied; title/artist always present. Called only on the main thread
from `CompanionService::onDeckLoaded`. The `/v1/tracks/:id` HTTP endpoint (needs
main-thread marshalling) is deferred to the T06 work with the search endpoint.
