# T06 — Library Search Endpoint (PRIMARY TARGET)

**Phase:** 2 · **Size:** M–L · **Blocked by:** T05 (reuses marshalling + serializer)

## Objective

`GET /v1/library/search?q=...&bpmMin=&bpmMax=&key=&limit=&offset=` returning
`SearchResult` per `03-api-spec.md` — the phone's library browser backend, using
Mixxx's own search grammar.

## Design references

`01-codebase-map.md` §5. Key pieces: `SearchQueryParser` (same grammar as the
desktop search box), `TrackCollection`/`TrackDAO`, main-thread-only DAO rule.

## Steps

1. `src/companion/libraryquery.h/.cpp`. Request struct: `{q, bpmMin, bpmMax, key,
   limit (default 50, max 200), offset}`. Convenience params compose onto `q` as
   `bpm:>=X bpm:<=Y key:K` (check the exact grammar `SearchQueryParser` accepts for
   ranges — read its unit tests in `src/test/searchqueryparsertest.cpp` first and
   mirror them).
2. On the main thread (queued from HTTP handler via the T05 marshalling utility):
   - Build `SearchQueryParser(pTrackCollection, searchColumns)` with the same
     default columns the library uses (find them where `SearchQueryParser` is
     constructed for the library feature — grep `new SearchQueryParser`).
   - `parseQuery(...)` → `QueryNode` → SQL WHERE; run a SELECT over the `library`
     joined with `track_locations` (mirror how `BaseTrackCache`/library models
     build their queries) with `LIMIT/OFFSET`, ordered by artist, title.
   - Produce `TrackSearchRow`s directly from the query columns (cheaper than
     hydrating full Track objects; do NOT load TrackPointers for search rows).
   - `total`: `COUNT(*)` capped at 1000 (`"total": 1000` means "≥1000").
3. Exclude hidden/purged tracks (mirror the standard library WHERE conditions —
   `mixxx_deleted`/hidden flags; copy from an existing library query).
4. Empty `q` with filters is valid; entirely empty query returns recent N by
   default (`ORDER BY id DESC LIMIT`), documented in the spec.
5. Timing: log query duration; if a broad search on a large library blocks the main
   thread > ~20 ms, note it in the completion report — the fallback (worker-thread
   read-only `QSqlDatabase` clone) is pre-approved in `02-architecture.md` but do
   not implement it speculatively.
6. Tests: unit — param composition and clamping. Integration — seeded test library
   (see how existing library tests build fixtures, e.g. `LibraryTest`): text match,
   bpm range, key filter, pagination, hidden-track exclusion, injection attempt
   (`q="'; DROP TABLE library;--"`) is safely parameterized.

## Acceptance criteria

- [ ] `curl "…/v1/library/search?q=bicep"` returns correct rows from a real library.
- [ ] `bpm:120-128` style queries and `bpmMin/bpmMax` params both work and agree.
- [ ] Pagination stable (no duplicates/gaps across pages while library unchanged).
- [ ] Search of a 10k-track library returns < 100 ms end-to-end on localhost.
- [ ] No SQL injection path (parameterized/escaped via the existing query
      infrastructure — no string-concatenated user input into SQL).

## Out of scope

Playlists/crates endpoints (Phase 3), `library.changed` events (Phase 3),
suggestions/compatibility scoring (Phase 5).
