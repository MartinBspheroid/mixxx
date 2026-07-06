# 01 — Verified Codebase Map (branch `2.6`, 2.6-beta-1913-g9ebccb8f20)

Every claim below was verified against this working tree. Line numbers drift —
**grep for the symbol before editing**. Paths are relative to the repo root.

## 1. Deck players and load events

`src/mixer/basetrackplayer.h`
- Signals (the primary event source for `deck.loaded`/`deck.unloaded`):
  - `void newTrackLoaded(TrackPointer pLoadedTrack);` (~L67)
  - `void trackUnloaded(TrackPointer pUnloadedTrack);` (~L68)
  - `void loadingTrack(TrackPointer pNewTrack, TrackPointer pOldTrack);` (~L69)
  - `void playerEmpty();` (~L73)
- `TrackPointer getLoadedTrack() const` — abstract at ~L45, `final` impl in
  `BaseTrackPlayerImpl` ~L93.

`src/mixer/playermanager.h`
- `static QString groupForDeck(int i)` (~L152) → `"[ChannelN]"` group strings
  (0-indexed input). Stem variant `groupForDeckStem(...)` exists (new in 2.6).
- `BaseTrackPlayer* getDeckBase(int deckIndex) const` (~L42/L113).
- `int numberOfDecks() const` (+ signal `numberOfDecksChanged(int)`).
- Load actions (for T07 remote actions — location-based slots are stable):
  - `void slotLoadLocationToPlayer(const QString& location, const QString& group, bool play);` (~L208)
  - `void slotLoadLocationToPlayerMaybePlay(...)` (~L209)
  - `void slotLoadToDeck(const QString& location, int deckNumber);` (~L217, 1-indexed)
  - `slotLoadToPreviewDeck`, `slotLoadToSampler`, `slotLoadLocationIntoNextAvailableDeck`.
- Caveat: `slotLoadTrackToPlayer` has two signatures depending on the `__STEM__`
  define — prefer the location-based slots, or handle the ifdef.

## 2. Control system (deck state values)

`src/control/controlobject.h`
- Static, **thread-safe from any thread**:
  - `static ControlObject* getControl(const ConfigKey& key, ...)` (~L28)
  - `static double get(const ConfigKey& key);` (~L89)
  - `static void set(const ConfigKey& key, const double& value);` (~L117)

`src/control/controlproxy.h`
- `get()`, `set()`, `getParameter()` documented "Thread safe, non-blocking".
- Subscribe to changes with `connectValueChanged(receiver, slot, connectionType)` —
  do **not** connect the `valueChanged` signal directly.
- **Thread rules (header ~L10–L17):** a `ControlProxy` must be created and deleted on
  the same thread (pending signals can segfault otherwise — upstream issue #7773);
  do not (re-)connect slots during runtime. Consequence for us: create all proxies
  on the CompanionService thread, parented to an object living on that thread, and
  use queued connections.

Confirmed per-deck controls (group from `PlayerManager::groupForDeck`):
- `play`, `playposition` (defined in `src/engine/enginebuffer.cpp` ~L127/L168;
  0.0 = start, 1.0 = end), `track_loaded` (~L189), `duration` (seconds), `rate`,
  `bpm`, `key`, `vu_meter` (`src/engine/enginevumeter.cpp` ~L21).
- `positionSeconds = playposition * duration`.

## 3. Track metadata and waveform accessors

`src/track/track.h` (all confirmed):
- `getTitle` ~L186, `getArtist` ~L188, `getAlbum` ~L190, `getAlbumArtist` ~L192,
  `getGenre` ~L223, `double getBpm()` ~L156 (+ `getBpmText`),
  `QString getKeyText()` ~L409 (+ `getKey()` returning ChromaticKey ~L408),
  `double getDuration()` ~L133, `int getRating()` ~L275,
  `int getTimesPlayed()` ~L267, `QString getLocation()` ~L97.
- Waveforms: `const ConstWaveformPointer& getWaveform() const;` ~L296 and
  `ConstWaveformPointer getWaveformSummary() const;` ~L299.
- `Track` objects are shared via `GlobalTrackCache`; hold `TrackPointer`
  (shared ptr), copy scalar fields into DTOs, do not retain long-lived references
  on the service thread.

`src/waveform/waveform.h`:
- Bands enum: `AllBand=0, Low=1, Mid=2, High=3, BandCount=4`; channels Left/Right.
- `WaveformData { WaveformFilteredData filtered; unsigned char stems[kMaxSupportedStems]; }`
  with `WaveformFilteredData { low, mid, high, all }` — all `unsigned char`.
  The `stems` array is **new in 2.6**.
- Accessors: `getDataSize()`, `get(i)`, `getLow/getMid/getHigh/getAll(i)`, `data()`,
  `getCompletion()`, `getAudioVisualRatio()`, `hasStem()`.
- Serialization exists: `QByteArray toByteArray() const;` + ctor from `QByteArray`
  (protobuf-backed, used by AnalysisDao). Our wire format is separate (see 03).

## 4. Waveform analysis and storage

`src/analyzer/analyzerwaveform.cpp`:
- `constexpr int mainWaveformSampleRate = 441;` (~L62)
- `constexpr int summaryWaveformSamples = 2 * 1920;` (~L64)  → 3840 samples.
- Summary size on the wire ≈ 3840 samples × 4 bands × uint8 ≈ 15 KB mono-mixed
  (stereo interleave doubles it; see 03-api-spec for the format we chose).

`src/library/dao/analysisdao.h` / `.cpp`:
- `enum AnalysisType { TYPE_UNKNOWN, TYPE_WAVEFORM, TYPE_WAVESUMMARY }` (~L16).
- `getAnalysesForTrackByType(TrackId, type)` ~L48, `saveAnalysis` ~L50,
  `getAnalysisStoragePath()` ~L61.
- Storage is hybrid: SQL table `track_analysis` holds metadata rows; waveform bytes
  are **zlib-compressed files on disk** named by analysis id under
  `getAnalysisStoragePath()`, read via `qUncompress`.
- Simplest export path: use the in-memory `Track::getWaveformSummary()` of the
  loaded track; fall back to AnalysisDao lookup for arbitrary library tracks.

## 5. Library layer and search

- `src/library/trackcollectionmanager.{h,cpp}` — `TrackCollection* internalCollection()`
  (~L41), `saveTrack(...)`.
- `src/library/trackcollection.{h,cpp}`, `src/library/dao/trackdao.{h,cpp}`,
  `dao/playlistdao.{h,cpp}`, `dao/cuedao.{h,cpp}`.
- Crates live in `src/library/trackset/crate/` (not `library/crate/`):
  `cratestorage.h` (`CrateStorage::selectCrates()` ~L282, `readCrateById/ByName`).
- Search infrastructure:
  - `src/library/searchqueryparser.{h,cpp}` —
    `SearchQueryParser(TrackCollection*, QStringList searchColumns)`;
    `parseQuery(...)` returns a `QueryNode` that renders to a SQL WHERE clause.
    Supports the same query grammar as the Mixxx search box (`bpm:>120`, `key:8A`,
    quoted phrases, `-negation`).
  - `src/library/basetrackcache.{h,cpp}` — in-memory column cache;
    `filterAndSort(const QSet<TrackId>&, ...)` (~L68).
- **Thread constraint (hard rule):** DAOs and `TrackCollection` use the SQLite
  connection owned by the main thread. All library queries from the companion
  service must be marshalled to the main thread via queued signal/slot (small,
  paginated result sets), or use a dedicated read-only `QSqlDatabase` clone on the
  service thread (acceptable for search SELECTs; never write).

## 6. Networking status in tree

- CMake links Qt component `Network` already (`CMakeLists.txt`, `QT_COMPONENTS` list
  ~L3568).
- **No server code exists anywhere in `src/`** — no `QTcpServer`, `QWebSocket`,
  `QHttpServer`. `src/network/` is outbound HTTP client helpers
  (`webtask`, `jsonwebtask` around `QNetworkAccessManager`, used by MusicBrainz /
  cover art). `src/broadcast/` is outbound Icecast/Shoutcast audio streaming.
- QtWebSockets / QtHttpServer are **not** in the build and may not be present in the
  prebuilt vcpkg environment — resolving this is task T01.

## 7. New-in-2.6 features relevant to us

- **Stems**: `src/track/steminfo.{h,cpp}`, `steminfoimporter.{h,cpp}`; `__STEM__`
  guards in PlayerManager; `WaveformData::stems[]`. Companion API v1 ignores stems
  but the DTOs reserve room (see 03).
- **Controller screens rendering** (prior art for waveform→image, not used in v1):
  `src/controllers/rendering/controllerrenderingengine.{h,cpp}` renders offscreen via
  `QOffscreenSurface` + `QQuickRenderControl` + `QQuickWindow`, emits `QImage` frames.
  Confirms the "export data, render on client" decision — Mixxx-side rendering is
  heavy machinery we don't need.
- **QML**: optional behind `if(QML)` in CMake; `Qml` component always linked (for
  QJSEngine). No `.qml` files under `src/`.

## 8. Service lifecycle home

`src/coreservices.{h,cpp}` — `class CoreServices : QObject`:
- Getters we need: `getPlayerManager()` (~L59), `getTrackCollectionManager()` (~L89),
  `getLibrary()` (~L85), `getSettingsManager()` (~L93).
- Lifecycle: `initialize(QApplication*)` (~L41) constructs managers as shared_ptrs
  (PlayerManager ~L568, TrackCollectionManager ~L616, Library ~L621);
  `finalize()` (~L125) resets them in reverse order.
- **CompanionService is constructed in `CoreServices::initialize` after PlayerManager
  and Library exist, stored as a member, and reset early in `finalize()`** (stop
  accepting connections before the things it observes are torn down).

## 9. Build system and platforms

- Dependencies come from a **prebuilt vcpkg environment** (`MIXXX_VCPKG_ROOT`,
  overlay ports/triplets). No `vcpkg.json` manifest, no `CMakePresets.json`.
- macOS arm64 handled explicitly (`CMAKE_SYSTEM_PROCESSOR MATCHES "arm64"`,
  `CMAKE_OSX_ARCHITECTURES arm64`). Generic/unknown triplets (Linux aarch64 /
  Raspberry Pi) go through the "set your own `VCPKG_TARGET_TRIPLET`" path; nothing
  blocks ARM Linux. Even wasm/Emscripten has a code path.
- No x86-only assumptions near any of the integration points above.
