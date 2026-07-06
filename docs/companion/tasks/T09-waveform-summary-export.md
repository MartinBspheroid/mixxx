# T09 — Waveform Summary Export + Cue Markers (post-MVP)

**Phase:** 3 · **Size:** M–L · **Blocked by:** T05

Deliberately after the MVP: the original plan's explicit warning is that waveform
work is the rabbit hole — do not let it precede communication/search/actions.

## Objective

`GET /v1/tracks/:id/waveform/summary` returning the `MXWF v1` binary blob, and
`GET /v1/tracks/:id/cues` returning cue/loop/intro/outro markers — everything a
client needs for an overview waveform with playhead and markers.

## Design references

`03-api-spec.md` §MXWF (the byte-exact contract) and `01-codebase-map.md` §3–4.
Summary waveform: 2×1920 = 3840 samples, bands all/low/mid/high as uint8
(`src/analyzer/analyzerwaveform.cpp`, `src/waveform/waveform.h`).

## Steps

1. `src/companion/waveformexporter.h/.cpp`:
   - Resolution order: loaded deck's `Track::getWaveformSummary()` (in-memory) →
     `AnalysisDao::getAnalysesForTrackByType(trackId, TYPE_WAVESUMMARY)` +
     `Waveform(QByteArray)` deserialize (main thread for DAO access; the byte
     array parse can move to the worker) → `202 analysis_pending`.
   - Guard `getCompletion()`/`getDataSize()` — partially analyzed waveforms exist;
     export only completed ones, else 202.
   - Encode MXWF via `QDataStream` LittleEndian, mono-mix = `max(L,R)` per band —
     the Waveform stores interleaved L/R as consecutive samples per
     `getDataSize()`; verify actual layout by reading `Waveform::getAll(i)` usage
     in the existing renderers (`src/waveform/renderers/`) before assuming.
     Document the verified layout in the completion note.
2. Cache encoded blobs (LRU, ~32 entries) keyed by trackId+analysis version on the
   worker thread — encoding is cheap but repeat requests (reconnects) are common.
3. `GET /v1/tracks/:id/cues`: main-thread read of the loaded track's cue list
   (`Track::getCuePoints()` — verify the getter name in track.h) or `CueDAO` for
   unloaded tracks. JSON: `{type: "hotcue"|"loop"|"intro"|"outro"|"maincue",
   index?, positionSeconds, lengthSeconds?, label?, color?}` — positions in the
   file are sample frames; convert via track sample rate (see how
   `CuePointer`/`CueInfo` stores positions — `mixxx::audio::FramePos`).
   **Update `03-api-spec.md` with the final cue schema.**
4. Add `waveform.summaryUrl` + `sampleCount`/`format` fields to `deck.loaded`
   (spec already reserves them).
5. Tests: golden MXWF fixture (byte-exact, catches endianness/layout regressions);
   pending-analysis path; cue conversion math (frames→seconds) against a fixture
   track; cache invalidation on re-analysis.
6. Verification page: extend HACKING.md with a 30-line HTML/JS canvas snippet that
   fetches the blob and draws the waveform — the human check that bands look right.

## Acceptance criteria

- [ ] Blob for an analyzed track decodes to a waveform visually matching Mixxx's
      overview (bands and shape) in the test page.
- [ ] Unanalyzed track → 202 with `analysis_pending`; after Mixxx analyzes it, the
      same URL returns the blob.
- [ ] Cues endpoint positions match Mixxx UI markers to the second.
- [ ] Encoding a summary takes < 5 ms; no main-thread stall > 1 ms per request
      beyond the DAO read.

## Out of scope

Main (441 samples/s) waveform export (future `/waveform/main`, format reserved),
stem bands, beatgrid endpoint (follow-up — note it in the completion report),
any server-side rendering.
