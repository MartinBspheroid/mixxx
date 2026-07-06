# 04 — Build System and Platform Strategy

## Goal

One codebase that compiles unchanged for:

1. **macOS arm64** — primary target *now* (development machine).
2. **Linux x86_64** — next (any laptop/desktop, CI).
3. **Linux aarch64 / Raspberry Pi** — later (headless/embedded Mixxx rig).

The companion module must never be the reason a platform fails to build.

## Rules for platform-agnostic companion code

- **Qt-only APIs** in `src/companion/`: QtCore, QtNetwork, QtWebSockets. No POSIX
  sockets, no `#ifdef Q_OS_*`, no platform headers.
- **No architecture assumptions**: no intrinsics, no raw pointer-cast serialization.
  Binary output through `QDataStream` with `setByteOrder(QDataStream::LittleEndian)`
  and explicit field writes (see MXWF spec in 03).
- **No new non-Qt C++ dependencies.** Every third-party lib added is a porting cost
  ×3 platforms via vcpkg. Qt is already ported everywhere Mixxx runs, including
  Raspberry Pi OS.
- Keep companion code behind the `[CompanionAPI] enabled` runtime flag *and* a CMake
  option `COMPANION_API` (default `ON`) so any platform can build with it off.

## Current state (verified)

- Qt components linked: Concurrent, Core, Gui, Network, OpenGL, PrintSupport, Qml,
  Sql, Svg, Test, Widgets, Xml (CMakeLists `QT_COMPONENTS`, ~L3568). Qt ≥ 6.2
  (Qt6 path) — actual env is expected to be ≥ 6.5.
- **QtWebSockets and QtHttpServer are not in the build**, and Mixxx uses a
  *prebuilt vcpkg environment* (no manifest mode): dependencies come from
  `MIXXX_VCPKG_ROOT` built from mixxxdj's vcpkg fork. If the prebuilt Qt there
  lacks `qtwebsockets`/`qthttpserver`, they must be added to the environment (or
  built into it locally) — this is the main build-system risk, resolved in T01.
- macOS arm64: explicit support (vcpkg triplet `arm64-osx`, min-version guards).
- ARM Linux: generic triplet path exists (`-DVCPKG_TARGET_TRIPLET=arm64-linux` with
  your own vcpkg env). No x86-only code near our integration points.

## Server library decision (T01 investigates, this is the default)

- **WebSocket: QtWebSockets (`QWebSocketServer`)** — mature, cross-platform, in the
  Qt release set since Qt 5. This is required; no fallback needed.
- **HTTP: two acceptable options, in preference order:**
  1. **QtHttpServer** (`Qt::HttpServer`) if the vcpkg env provides it (Qt ≥ 6.4;
     API stable from 6.8). Cleanest routing.
  2. **Minimal hand-rolled HTTP/1.1 on `QTcpServer`** (~300 lines: request-line +
     headers parse, GET/POST only, fixed routes, `Connection: close`). Zero new
     dependencies; entirely sufficient for our tiny surface. Choose this if
     QtHttpServer is missing or still tech-preview in the env's Qt version.
- The HTTP layer is isolated behind a small internal interface in
  `companionserver.h` so swapping (2)→(1) later is a contained change.

## CMake changes (made in T01)

```cmake
option(COMPANION_API "Build the companion network API" ON)

if(COMPANION_API)
  list(APPEND QT_COMPONENTS WebSockets)   # + HttpServer if chosen
  target_sources(mixxx-lib PRIVATE src/companion/...)
  target_compile_definitions(mixxx-lib PUBLIC __COMPANION__)
  target_link_libraries(mixxx-lib PRIVATE Qt${QT_VERSION_MAJOR}::WebSockets)
endif()
```

`CoreServices` guards construction with `#ifdef __COMPANION__` (mirrors the existing
`__STEM__`/`__BROADCAST__` pattern in this codebase).

## Per-platform build notes

### macOS (now — T11)

Standard Mixxx macOS build per `CONTRIBUTING.md`/wiki: prebuilt vcpkg env for
arm64-osx, then

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DMIXXX_VCPKG_ROOT=<env path> ..
cmake --build build -j
```

T11 verifies: builds clean, service boots, all MVP endpoints answer, `mixxx-test`
targets for companion pass, and (with `COMPANION_API=OFF`) the tree still builds.

### Linux x86_64 (next — T12)

Distro-Qt or vcpkg env build; add `qtwebsockets` dev package (`qt6-websockets-dev`
on Debian/Ubuntu) when building against system Qt. CI: add a GitHub Actions job to
our fork building Linux with `COMPANION_API=ON` so the platform never rots.

### Raspberry Pi / Linux aarch64 (later — T12)

- Target Raspberry Pi OS (64-bit) or Ubuntu Server arm64 on Pi 4/5.
- Two routes, in preference order:
  1. **Native/distro build on the Pi** with system Qt6 (RasPi OS bookworm ships
     Qt 6.4+): slowest compile, fewest surprises. Use the hand-rolled HTTP option
     if that Qt lacks a stable QtHttpServer.
  2. Cross-compile with vcpkg `arm64-linux` triplet.
- Audio backend and GUI concerns (KMS/EGL vs X11) are stock-Mixxx issues, not
  companion issues; the companion service itself is headless-friendly and is a key
  reason the Pi target is attractive (Pi as silent Mixxx engine + phone as UI).

## Rebase policy against upstream

The fork must stay rebasable on upstream `2.6`:

- All new files under `src/companion/` + docs; modifications limited to
  `CMakeLists.txt` (one guarded block) and `src/coreservices.{h,cpp}` (one guarded
  member + two call sites).
- Weekly (or per-work-session) `git fetch upstream && git rebase upstream/2.6` on
  the `companion-api` branch. Conflicts should only ever appear in the two touched
  files.
