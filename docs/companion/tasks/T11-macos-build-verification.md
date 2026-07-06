# T11 — macOS Build + End-to-End Verification Pass

**Phase:** runs alongside everything (first full pass after T03; final pass after
T07) · **Size:** S–M recurring

## Objective

Prove the fork builds and runs on the primary target (macOS arm64) and that the MVP
feature set works end-to-end from a second device, so regressions are caught while
the diff is small.

## Environment setup (once)

Follow the Mixxx wiki "Compiling on macOS": install the prebuilt vcpkg environment
for arm64-osx matching the 2.6 branch (`MIXXX_VCPKG_ROOT`), Xcode CLT, CMake ≥3.21.
If T01 added qtwebsockets to the env, use the updated env; document the env
version/hash in the completion note so builds are reproducible.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DMIXXX_VCPKG_ROOT=<path> -DCOMPANION_API=ON
cmake --build build -j
```

## Verification checklist (repeat per pass; record results in this file)

Build matrix:
- [ ] `COMPANION_API=ON` builds clean (no new warnings in companion code with the
      project's warning flags)
- [ ] `COMPANION_API=OFF` builds clean
- [ ] `mixxx-test` companion suites pass

Runtime (with a real music library loaded):
- [ ] Service disabled by default; enabling via config works; port logged
- [ ] `/v1/status` from localhost and (post-T08, after pairing) from a phone browser
- [ ] WS: song info + live position on second device; drift within tolerance
- [ ] Search from phone returns library results
- [ ] Load-to-deck + play/pause from phone work; guards behave
- [ ] Quit Mixxx with clients connected: clean exit < 5 s, no crash dialog
- [ ] 2-hour soak during a practice mix: no leak (Activity Monitor), no dropouts

Audio safety:
- [ ] With service enabled + 2 clients, no increase in buffer underruns
      (`--developer` mode stats) vs disabled baseline

## Out of scope

Packaging/DMG/signing (only needed when distributing; the fork is self-built for
now), CI setup (T12 bundles the Linux CI job).
