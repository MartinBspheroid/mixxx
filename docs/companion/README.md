# Mixxx Companion API — Plan Documents

This folder is the single source of truth for the **Companion API** project: a narrow,
native service inside this Mixxx fork that exposes deck state, loaded-track metadata,
library search, waveform summaries, and safe remote actions to companion clients
(a phone/tablet app first) over the local network.

The fork is based on the **`2.6` branch (2.6-beta)** of `mixxxdj/mixxx`. Upstream remote
is `upstream`, our fork is `origin` (`MartinBspheroid/mixxx`).

## Project priorities (in order)

1. **Communication layer** — WebSocket + HTTP service inside Mixxx. This is the key
   deliverable; everything else builds on it.
2. **Song information** — what is loaded/playing on each deck, position, BPM, key,
   remaining time, pushed live to the phone.
3. **Library search** — full-text + filtered search of the Mixxx library from the phone.
4. **Remote function calls** — safe actions from the phone (load track to deck,
   play/pause, etc.).
5. **Waveform overview export** — secondary; explicitly *not* part of the MVP.
6. **Thorough documentation** — every task ships with API docs and a developer README
   update. See `05-testing-and-docs.md`.

## Platform policy

Primary build target **today is macOS (arm64)**. The implementation must remain
**platform agnostic** so the same code compiles for Linux x86_64 and Raspberry Pi
(Linux aarch64) later. Concretely: Qt-only APIs, no platform `#ifdef`s in companion
code, no architecture intrinsics, endianness-explicit binary formats. Details and
rationale: `04-build-and-platforms.md`.

## Document map

| Doc | Contents |
|---|---|
| `00-overview.md` | Vision, role split, architecture summary, decision log |
| `01-codebase-map.md` | Verified integration points in this branch (files, classes, signals) |
| `02-architecture.md` | CompanionService module design, threading model, lifecycle |
| `03-api-spec.md` | HTTP + WebSocket API contract, event schemas, DTOs, binary waveform format |
| `04-build-and-platforms.md` | CMake/Qt module changes, macOS/Linux/RasPi strategy |
| `05-testing-and-docs.md` | Testing strategy and documentation standards |
| `tasks/` | One self-contained implementation plan per task, sized for a single agent |

## How to work on a task (for agents)

1. Read `00-overview.md`, `02-architecture.md`, and `03-api-spec.md` first.
2. Read your task file in `tasks/`. Each task lists: objective, prerequisites,
   files to create/modify, exact steps, acceptance criteria, and out-of-scope items.
3. Line numbers in these docs were verified on `2.6-beta-1913-g9ebccb8f20`; they drift.
   Always re-locate symbols with grep before editing.
4. Do not change behavior outside `src/companion/` except where a task explicitly
   says so (CMake, `CoreServices`, preferences).
5. Hard rules (from `02-architecture.md`): never block the audio engine, respect
   main-thread-only DAO access, read-only endpoints before write actions, default
   bind `127.0.0.1`, LAN exposure requires pairing.
6. Update `03-api-spec.md` and the task's "Documentation" checklist as part of the
   task — a task is not done until its docs are done.

## Task order and dependencies

```
T01 build system (Qt WebSockets/HttpServer)      ── blocks everything
T02 CompanionService skeleton + lifecycle        ── blocks T03..T09
T03 /v1/status endpoint                          ── first end-to-end proof
T04 deck state publisher (WS events + song info) ── PRIMARY TARGET
T05 track metadata DTO (deck.loaded payload)     ── PRIMARY TARGET (with T04)
T06 library search endpoint                      ── PRIMARY TARGET
T07 remote actions (load/play/pause from phone)  ── PRIMARY TARGET
T08 pairing + security (needed before LAN use)
T09 waveform summary export (v2, after MVP)
T10 client SDK notes + Android MVP plan
T11 macOS build + manual verification pass
T12 Linux + Raspberry Pi port pass
```

T03–T07 constitute the MVP ("communication is the key"). T09 is deliberately after
the MVP: the original plan's advice is that waveform replication must not be the
first milestone.
