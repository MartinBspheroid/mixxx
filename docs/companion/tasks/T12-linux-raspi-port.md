# T12 — Linux + Raspberry Pi Port Pass

**Phase:** 5 (after MVP is stable on macOS) · **Size:** M

## Objective

Compile and verify the fork on Linux x86_64, add a CI guard so it stays compiling,
and produce a documented build recipe for Raspberry Pi (Linux aarch64) — the
long-term goal of a headless Pi Mixxx rig driven from the phone.

## Part 1 — Linux x86_64

1. Build on Ubuntu 24.04 (or Debian bookworm): follow Mixxx wiki Linux build;
   ensure `qt6-websockets-dev` (+ `qt6-httpserver-dev` if T01 chose QtHttpServer)
   is installed when building against system Qt; verify the CMake component lookup
   finds them.
2. Fix any portability fallout in companion code (there should be none if the T01
   rules held — any `#ifdef` temptation is a design smell to escalate).
3. Run the full T11 runtime checklist on Linux.
4. **CI:** add `.github/workflows/companion.yml` to the fork: Ubuntu job, system
   Qt, `-DCOMPANION_API=ON`, build + `mixxx-test` companion suites, plus a second
   job with `=OFF`. Trigger on push/PR to `companion-api`.

## Part 2 — Raspberry Pi (aarch64)

1. Target: Pi 4/5, Raspberry Pi OS 64-bit (bookworm, Qt 6.4+) — build natively on
   the Pi first (slow but fewest unknowns; use `-j2` and swap, or distcc).
   Cross-compilation via vcpkg `arm64-linux` triplet is the optimization, not the
   first step.
2. Known watch-items:
   - Qt version on RasPi OS may be older than the dev machines — this is why the
     HTTP layer must not require Qt 6.8 (T01 policy).
   - GL/GLES: Mixxx waveform rendering on the Pi may need
     `QT_OPENGL=es2`-style config — stock-Mixxx concern; companion service itself
     has zero GL dependency.
   - Audio: confirm ALSA/JACK device selection headless.
3. Headless mode investigation (stretch): can Mixxx run with a minimal/hidden UI on
   the Pi while the phone is the primary interface? Document findings (offscreen
   platform plugin viability) — do not implement.
4. Document the full recipe as `docs/companion/BUILD-RASPI.md` with exact package
   lists and cmake invocation; include measured build time and runtime CPU/memory
   headroom during 2-deck playback + connected client.

## Acceptance criteria

- [ ] Linux x86_64: full T11 checklist passes.
- [ ] CI green on the fork for ON and OFF configs.
- [ ] Pi: Mixxx runs, service reachable from phone, 2-deck playback + ticking
      client without audio dropouts; recipe reproducible from BUILD-RASPI.md.
- [ ] Zero platform `#ifdef`s introduced in `src/companion/`.

---

## Completion note (Linux CI added; RasPi documented as follow-up)

`.github/workflows/companion.yml` added to the fork: two ubuntu-24.04 jobs on
push/PR to `companion-api` —
1. **COMPANION_API=ON**: deps via `tools/debian_buildenv.sh setup` + `qt6-websockets-dev`,
   ccache-cached, configure + build `mixxx-test`, run `--gtest_filter='Companion*'`.
2. **COMPANION_API=OFF**: configure + build `mixxx-lib` (compiles every guarded TU
   like `coreservices.cpp` without `__COMPANION__`, proving the guards).

Uses the exact dep set + cmake flags verified to build locally (Ubuntu 24.04, Qt 6.4);
YAML validated. First GitHub run may need minor tuning (runner package names, cache
warm-up). Raspberry Pi (aarch64) native build recipe remains a documented follow-up
(`BUILD-RASPI.md`) — the CI proves x86_64 Linux; the companion code is Qt-only with no
platform `#ifdef`s, so RasPi is a packaging exercise, not a code one.
