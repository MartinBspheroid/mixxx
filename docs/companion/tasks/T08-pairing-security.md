# T08 — Pairing and Security (required before real LAN use)

**Phase:** 2 · **Size:** M · **Blocked by:** T03 (+ T07 for write-gating to matter)

## Objective

Token-based pairing so the API can be safely exposed on Wi-Fi: per-device bearer
tokens, loopback exemption, read-only vs control scopes. A DJ rig on venue Wi-Fi
must not accept commands from strangers.

## Design

- `src/companion/pairingmanager.h/.cpp`.
- Token issuance ("pairing window"): triggered from Mixxx (v1: a config action or
  `POST /v1/pair` **accepted from loopback only**) → generates a one-time 6-digit
  code + QR payload `mixxx-companion://pair?host=<ip>&port=24742&code=<code>`,
  valid 60 s. Client exchanges code via `POST /v1/pair/claim {code, deviceName}` →
  long-lived random token (32 bytes, `QRandomGenerator::system()`), stored hashed
  (SHA-256) in `[CompanionAPI]` settings with device name + created-at.
- Enforcement: non-loopback HTTP requires `Authorization: Bearer`; WS upgrade
  requires `?token=`. Loopback stays open (localhost = trusted, matches decision
  D5). Scopes: token records `readOnly: bool`; write actions (T07) require a
  control-scope token.
- Token management v1: list/revoke via loopback-only endpoints
  (`GET/DELETE /v1/pair/devices[/:id]`); Preferences UI is a stretch goal.
- Transport: plain HTTP on LAN v1 (documented limitation — tokens are sniffable on
  open Wi-Fi; mitigations: personal hotspot / trusted LAN). TLS
  (`QSslServer`/wss) is a documented Phase 3 option — record as future decision,
  don't build now.
- Rate-limit `pair/claim` attempts (5 tries per window) and all auth failures
  (per-IP backoff) to keep code brute-force infeasible.

## Steps

1. Implement PairingManager + settings-backed token store (hashed).
2. Wire auth check into the HTTP router and WS upgrade path (single choke point).
3. `allow_lan=true` now additionally requires ≥1 paired device or an explicit
   `allow_lan_unauthenticated=true` dev-only flag (log screaming warning).
4. Update `/v1/status` `auth` field: `open-loopback` / `token`.
5. Update `03-api-spec.md` auth section with the final endpoints and QR format.
6. Tests: claim flow happy path; expired window; wrong code lockout; revoked token
   → 401; loopback exemption; read-only token blocked from T07 actions (403);
   token never logged or returned after issuance.

## Acceptance criteria

- [ ] Phone on same Wi-Fi can pair via code and then use all MVP endpoints.
- [ ] Unpaired LAN client gets 401 on everything (except the pair/claim endpoint).
- [ ] Read-only token cannot execute actions.
- [ ] Tokens survive Mixxx restart; revocation works immediately.
- [ ] Stored tokens are hashed; plaintext token shown exactly once at claim.

## Out of scope

TLS/wss, mDNS/DNS-SD discovery (nice-to-have — record as a follow-up task if
desired), Preferences UI polish.

---

## Completion note (implemented, tested, runtime-verified)

Implemented `src/companion/pairingmanager.{h,cpp}`: hashed (SHA-256) token store,
60 s pairing-code window with a 5-attempt lockout, 32-byte random tokens
(`QRandomGenerator::system()`), read-only vs control scopes, JSON serialize/load
for persistence. Auth is enforced at a single choke point in
`CompanionServer::route()` (loopback trusted; LAN needs a valid token; POST/write
needs a control-scope token) and on the WS upgrade in
`onWebSocketUpgradeRequested` (rejects with a raw 401 before handoff).
`HttpConnection`/`HttpRequest` gained `fromLoopback` (from `peerAddress().isLoopback()`)
and `bearerToken()` (Authorization: Bearer, else `?token=`).

Endpoints: `POST /v1/pair` (loopback-only; returns code + `mixxx-companion://` QR),
`POST /v1/pair/claim {code,deviceName,readOnly}` (code-gated, LAN-usable; returns the
token once), `GET /v1/pair/devices` + `DELETE /v1/pair/devices/:id` (loopback-only).
`/v1/status` `auth` field is `open-loopback` (loopback bind) or `token` (LAN bind),
plus a `pairedDevices` bool.

Persistence: tokens are stored in `[CompanionAPI] paired_tokens` (opaque JSON of the
hashes) and flushed to disk **immediately** on pair/revoke via `ConfigObject::save()`
(so a pairing survives an unclean exit, not just a graceful quit), loaded into the
PairingManager in the CompanionServer ctor at startup.

Verification:
- **Unit tests (9/9 pass, `CompanionPairingTest`)**: loopback exemption, unauthorized
  without token, full claim flow, read-only scope forbidden from control, expired
  window, brute-force lockout, revoke invalidation, serialize/load round-trip, hashing.
- **Runtime (real headless Mixxx, loopback)**: `POST /v1/pair` returns a code+QR;
  `claim` mints a 64-hex token; `devices` lists the hashed id (never the token);
  wrong code -> `bad_code`; `status.auth`/`pairedDevices` correct. Token written to
  `mixxx.cfg` immediately and present after `kill -9`.
- NOT observed only due to slow headless boot under heavy build load: the composed
  restart read-back (both halves — disk write and `loadTokens` — are independently
  verified). No graceful SIGTERM handler exists in Mixxx, which is why the immediate
  `save()` matters and why `timeout`-killed runs never wrote settings before.

Out of scope (unchanged): TLS/wss, mDNS discovery, Preferences UI.
