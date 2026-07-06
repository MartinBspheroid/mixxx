# 05 — Testing Strategy and Documentation Standards

## Testing strategy

Mixxx has a GoogleTest suite (`src/test/`, target `mixxx-test`). Companion tests
follow the same conventions.

### Unit tests (in `src/test/companion/`)

| Area | Test |
|---|---|
| TrackSerializer | Track fixture → exact expected JSON (field names are API contract) |
| MXWF encoder | golden binary: header layout, little-endian, sizes; round-trip decode |
| Search param parsing | `q`/`bpmMin`/`bpmMax`/`key`/`limit`/`offset` → parser input; clamping |
| Action whitelist | unknown action → `action_not_allowed`; body validation |
| Generation counter | load/unload sequences produce monotonic generations per deck |
| HTTP parser (if hand-rolled) | request-line/header edge cases, oversized requests rejected |
| Auth | token required off-loopback, not on loopback; bad token → 401 |

### Integration tests

- Spin up `CompanionServer` on port 0 (ephemeral) inside a test, drive it with
  `QWebSocket`/`QNetworkAccessManager` from the test itself: connect → `hello` →
  receive replayed `deck.loaded` → tick flow with a fake publisher.
- Thread teardown test: start/stop the service 100× — no leaks, no hangs
  (this catches the ControlProxy-thread and QThread-quit bugs early).

### Manual verification (each task lists its own; baseline below)

```sh
# from a second machine or the phone's browser:
curl http://<laptop>:24742/v1/status
websocat ws://<laptop>:24742/ws/v1     # watch deck.loaded / deck.tick while DJing
curl "http://<laptop>:24742/v1/library/search?q=bicep&limit=5"
curl -X POST http://<laptop>:24742/v1/decks/1/load -d '{"trackId":123}'
```

Latency check: tick timestamps vs wall clock across Wi-Fi; target visual tolerance
±30–80 ms. Soak check: leave connected through a 2-hour session; memory flat.

### Performance guardrails

- Tick serialization budget: < 0.1 ms per deck (it runs 10–20×/s on the worker
  thread; it must stay invisible).
- No measurable change to Mixxx audio dropout behavior with service enabled and a
  client connected (compare `--developer` stats with/without).

## Documentation standards ("thorough documentation" is a project goal)

1. **`03-api-spec.md` is the contract.** Any endpoint/event/DTO change lands in the
   same commit as the code. The spec never lags the implementation.
2. **Every task file gets a completion note** appended at the bottom when done:
   what was built, deviations from the plan and why, follow-ups discovered.
3. **Code documentation:** every class in `src/companion/` has a header comment
   stating its thread affinity ("lives on worker thread", "main thread only") and
   ownership. Match surrounding Mixxx style; no redundant comments.
4. **Developer onboarding:** `docs/companion/HACKING.md` (written in T03 once the
   first endpoint works) — how to enable the service, config keys, how to watch
   events, how to run companion tests. Kept under 2 screens.
5. **Client-facing docs:** T10 produces `docs/companion/CLIENT.md` — everything an
   app developer needs without reading C++: connect, auth, event handling,
   extrapolation math, MXWF decoding (with a reference decoder snippet in
   TypeScript and Kotlin).
6. **Decision log:** new architectural decisions get a row in `00-overview.md`'s
   decision log table (D11, D12, ...) rather than being buried in commit messages.
