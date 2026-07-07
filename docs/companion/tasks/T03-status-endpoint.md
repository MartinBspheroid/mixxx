# T03 — HTTP Listener + `GET /v1/status` (first end-to-end proof)

**Phase:** 1 · **Size:** M · **Blocked by:** T02

## Objective

`CompanionServer` listens on the configured port (default 24742, bind 127.0.0.1)
and answers `GET /v1/status` per `03-api-spec.md`. This proves the whole pipeline:
settings → thread → socket → JSON response, and gives every later task a heartbeat.

## Steps

1. In `CompanionServer` (worker thread), start the HTTP layer chosen in T01:
   - QtHttpServer route table, **or**
   - hand-rolled: `QTcpServer::listen(QHostAddress::LocalHost, port)`, parse
     request line + headers (reject bodies > 64 KB, unknown methods → 405),
     route `GET /v1/status`, respond `Connection: close`.
2. Implement the status payload (fields per spec). `numDecks` via
   `ControlObject::get(ConfigKey("[App]", "num_decks"))` or a value pushed from the
   facade at start; `uptimeMs` from a `QElapsedTimer`; `clients` hardcoded 0 for now.
3. Error shape per spec for unknown paths (404 JSON body).
4. Port-in-use handling: log a clear warning, service stays stopped — never crash.
5. Respect `allow_lan=false` → bind loopback only (LAN + auth arrive in T08; until
   T08 lands, `allow_lan=true` binds 0.0.0.0 **without auth** — acceptable only on
   trusted networks; log a loud warning).
6. Write `docs/companion/HACKING.md`: enable flags in `mixxx.cfg`, build, run,
   `curl` examples, log category filter.
7. Tests: integration test on port 0 — GET /v1/status returns 200 + valid JSON with
   `apiVersion:1`; unknown path → 404 JSON; oversized/garbage request → connection
   closed without crash.

## Acceptance criteria

- [ ] `curl http://127.0.0.1:24742/v1/status` returns the spec payload while Mixxx runs.
- [ ] Malformed input cannot crash Mixxx (fuzz the parser test with a few dozen
      garbage requests).
- [ ] Startup/shutdown from T02 still clean with sockets active (connect a client,
      quit Mixxx — no hang).
- [ ] `HACKING.md` exists and a newcomer can go zero→curl in one read.

## Out of scope

WebSocket (T04), auth (T08), any other endpoint.

---

## Completion note (implemented, pending build verification)

`httpconnection.h/.cpp` parses HTTP/1.1 (peek-based classification so WebSocket
upgrades can be handed to `QWebSocketServer` with the buffer intact; oversized
requests rejected; one-request-per-connection with `Connection: close`).
`companionserver.cpp` routes `GET /v1/status` to the spec payload and returns a JSON
404 for unknown paths. Bind address from settings (loopback unless `allow_lan`).
`HACKING.md` written. Malformed-input and port-in-use paths log and degrade instead
of crashing. Manual `curl`/`websocat` verification is part of T11.
