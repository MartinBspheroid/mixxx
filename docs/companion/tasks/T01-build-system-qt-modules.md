# T01 — Build System: Qt WebSockets/HTTP Modules + COMPANION_API Option

**Phase:** 1 (blocks all other tasks) · **Size:** S–M · **Touches:** CMake only

## Objective

Make `QWebSocketServer` linkable in this tree behind a `COMPANION_API` CMake option,
decide the HTTP layer (QtHttpServer vs hand-rolled QTcpServer), and prove it with a
compiled smoke file.

## Prerequisites

Read `04-build-and-platforms.md`. A working local Mixxx build environment
(vcpkg env or system Qt) must exist first — if it doesn't, setting it up per the
Mixxx wiki/CONTRIBUTING.md is part of this task.

## Steps

1. Inspect the available Qt in the build environment:
   `find <qt prefix> -name "*WebSockets*"` / check for `Qt6WebSockets` and
   `Qt6HttpServer` CMake config packages, and note the Qt version.
2. If `qtwebsockets` is missing from the vcpkg environment: add/build it
   (mixxxdj vcpkg env: add the port and rebuild the env; system Qt: install the
   dev package). Document the exact commands used in the completion note.
3. Decide HTTP layer per the policy in `04-build-and-platforms.md`
   (QtHttpServer only if present and Qt ≥ 6.8; otherwise hand-rolled). Record the
   decision as D11 in `00-overview.md`.
4. Edit `CMakeLists.txt`:
   - `option(COMPANION_API "Build the companion network API" ON)`
   - Inside `if(COMPANION_API)`: append `WebSockets` (and `HttpServer` if chosen)
     to `QT_COMPONENTS` *before* the `find_package(Qt...)` call (~L3568), add
     `__COMPANION__` compile definition, link `Qt::WebSockets` to `mixxx-lib`.
5. Add smoke file `src/companion/companionsmoke.cpp` (temporary, removed in T02):
   instantiates a `QWebSocketServer` in a function, proving compile+link.
6. Build with `-DCOMPANION_API=ON` and `=OFF`; both must succeed.

## Acceptance criteria

- [ ] `cmake -DCOMPANION_API=ON` configures and builds; `QWebSocketServer`
      compiles/links in `src/companion/`.
- [ ] `cmake -DCOMPANION_API=OFF` builds an unchanged Mixxx (no companion sources).
- [ ] HTTP-layer decision recorded in `00-overview.md` decision log + this file's
      completion note, with the Qt version found.
- [ ] No changes outside `CMakeLists.txt` and `src/companion/`.

## Out of scope

Any actual server logic (T02/T03); preferences; CI.
