# Kinoklip — Master Plan (v0.1, overnight build)

> "Kinoklip" is a **placeholder name**. Renaming = change `APP_NAME`/`APP_ID` in `CMakeLists.txt`
> and `src/app/AppInfo.h`. Nothing else should hard-code the name.

A non-linear video editor written from scratch in C++/Qt 6, whose **Edit page layout is inspired by
DaVinci Resolve** (panel arrangement + common NLE shortcuts). No Resolve code, binaries, assets,
icons, names or stylesheets are used or inspected. Colour grading is explicitly **out of scope**
for v0.1 (a disabled "Color" page tab is reserved for later).

---

## 1. Goals for the morning (definition of done)

1. `~/cutwise/dist/Kinoklip-x86_64.AppImage` — double-click runnable on KDE Plasma / any modern
   x86_64 Linux (glibc ≥ 2.39, e.g. KDE Linux, KDE neon 24.04, Fedora 40+, Arch). Copy on `~/Desktop`.
2. You can: create a project → import clips → cut them on a multi-track timeline → animate
   transform/opacity/volume with keyframes → add simple effects, transitions and text titles →
   play back with audio → export an MP4.
3. `morning-report.md`: what works, what is known-broken, what's next.

## 2. Non-goals (v0.1)
Color page/grading, node compositing (Fusion-like), Fairlight-like audio DAW, multicam, proxies,
GPU effects, hardware decode, speed ramps, plugins (OFX), collaboration, Windows/macOS builds
(the code is *prepared* for them, nothing more).

---

## 3. Tech stack

**Priority: performance first** (then correctness, then readability — compensated with thorough
comments; see docs/coding-style.md). Dev machine: Intel UHD 630 + NVIDIA GTX 1650 (driver 580).
**Target machine: ThinkPad T14s Gen 2 — i5-1145G7 (4C/8T), Intel Iris Xe (Tiger Lake, iHD VAAPI driver),
KDE Linux (Wayland).** ⇒ VAAPI is the primary hw path; must play 1080p smoothly (4K where hw decode allows).
AppImage bundles the Qt Wayland plugin but uses the host's libva/libGL/libEGL/libvulkan/libdrm + drivers.
| Area | Choice | Why |
|---|---|---|
| Language | C++20, CMake ≥ 3.24, Ninja | portable to Win/macOS later |
| UI | Qt 6 Widgets (**6.4.2 on this machine — do not use 6.5+ APIs**) | native on Plasma |
| Decode/encode | FFmpeg 6.1 libs (libavformat/codec/util/swscale/swresample) in-process | no external binaries |
| Audio out | QtMultimedia `QAudioSink` | cross-platform |
| Compositing | **GPU: OpenGL 3.3 core** (offscreen context on render thread, shared with viewer `QOpenGLWidget`s); effects/transitions/YUV→RGB as GLSL shaders; text rasterised once with QPainter → texture | fast on every vendor, portable to Win/macOS |
| HW decode | FFmpeg hwaccel: NVDEC/CUDA (NVIDIA), VAAPI (Intel/AMD), Vulkan; auto-select + SW fallback; later D3D11VA (Win), VideoToolbox (macOS) | |
| HW encode | NVENC (NVIDIA), VAAPI/QSV (Intel/AMD) H.264/HEVC/AV1; fallback libx264 | |
| Project files | JSON (`.kkproj`) | diffable, easy |
| Undo | snapshot-based (`QUndoStack` storing before/after `TimelineData`) | robust with many authors |
| Packaging | AppImage via linuxdeploy + Qt plugin | single file |
| Tests | QtTest, `QT_QPA_PLATFORM=offscreen` UI smoke tests, ffmpeg-generated fixtures | headless |

### Cross-platform readiness (no work yet, just rules)
- Only Qt + FFmpeg + std. No POSIX/X11/Wayland calls outside `src/platform/`.
- Paths via `QStandardPaths`; files via `QFile`/`QDir`.
- `packaging/linux/` real; `packaging/windows/` and `packaging/macos/` hold README stubs
  (windeployqt / macdeployqt + notarization notes).
- No compiler-specific extensions; must build with GCC, Clang and MSVC in principle.

---

## 4. UI layout (Resolve-inspired, own look)

Pages (tabs on a bottom page bar): **Media · Edit · Deliver** active; **Color** shown disabled
("coming later"). Shortcuts Shift+2 / Shift+4 / Shift+8 (Shift+5 reserved for Color).

**Edit page:**
```
┌─────────────────────────────────────────────────────────────────────────────┐
│ Menu bar                                                                    │
│ [Media Pool][Effects][Edit Index]      Project name       [Mixer][Metadata][Inspector] │  ← panel toggles
├──────────────┬────────────────────────────┬────────────────────┬────────────┤
│ Media Pool   │   Source Viewer            │  Timeline Viewer   │ Inspector  │
│ (bins tree + │   (clip, I/O marks)        │  (program output)  │ (tabs:     │
│  clip grid/  │   timecode, transport      │  timecode, JKL     │  Video /   │
│  list)       │                            │                    │  Audio /   │
│ Effects lib  │                            │                    │  Effects / │
│ (toggle)     │                            │                    │  Title /   │
│              │                            │                    │  Transition)│
├──────────────┴────────────────────────────┴────────────────────┴────────────┤
│ Timeline toolbar: [Select A][Trim T][Blade B] | [Snap N][Link] | zoom −/+  │
│ ruler + playhead + in/out                                                    │
│ V2 ▕ track header ▏ clips…                                                  │
│ V1 ▕               ▏ clips… (thumbnails, keyframe dots, fx badge)            │
│ A1 ▕               ▏ clips… (waveforms, volume line)                         │
│ A2 ▕               ▏                                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                [ Media ]   [ Edit ]   [ Color (disabled) ]   [ Deliver ]     │ ← page bar
└─────────────────────────────────────────────────────────────────────────────┘
```
- Built with nested `QSplitter`s (sizes persisted in `QSettings`), panels toggled from the
  interface toolbar. Single viewer mode toggle (hide source viewer).
- **Media page**: file-system browser (left) + preview viewer (centre) + media pool (bottom) +
  metadata (right).
- **Deliver page**: render settings (left), viewer (centre), render queue (right), mini timeline.
- **Look: identical on every OS, modelled on the latest KDE Plasma 6 Breeze.** The app always uses
  its own built-in theme (`src/app/Theme.*`: Fusion base + palette + QProxyStyle/QSS) reproducing
  current Breeze Dark (default) and Breeze Light: colour scheme values, 5 px rounded corners, flat
  tool buttons, accent-blue focus rings and selections, Breeze metrics/spacing, Noto Sans-like font
  stack. Icons: a bundled Breeze icon subset (LGPL-3.0, attributed), never the host icon theme, so
  Linux/Windows/macOS look the same. Host platform themes are ignored. Everything is verified
  against the latest Breeze (screenshots compared with Plasma 6 Breeze references).
  Editor-specific visuals (timeline clips, viewer chrome) use colours derived from the palette.

---

## 5. Architecture & directory layout
```
kinoklip/
  CMakeLists.txt            globs src/**; builds static lib kinoklip_core + exe + tests
  src/main.cpp              app entry; also CLI: --render <proj> <out.mp4>, --screenshot <page> <png>
  src/app/                  Application, MainWindow, PageBar, Theme, Preferences, AppInfo.h
  src/core/                 data model, edit operations, undo, selection, serialization, effect registry
  src/media/                FFmpeg: probe, VideoDecoder, AudioDecoder, MediaCache (thumbs, waveforms)
  src/render/               Compositor (frame @ time), effects, transitions, titles, AudioMixer
  src/playback/             PlaybackEngine (clock, prefetch, QAudioSink, JKL shuttle)
  src/ui/common/            shared widgets (TimecodeLabel, SliderSpin, KeyframeButton, Icons)
  src/ui/mediapool/         MediaPoolPanel, bins, MediaBrowser (Media page)
  src/ui/timeline/          TimelineWidget, ruler, track headers, tools, snapping
  src/ui/viewer/            ViewerWidget, SourceViewer, TimelineViewer, transport bar
  src/ui/inspector/         InspectorPanel (generic param editors + keyframe controls)
  src/ui/effects/           EffectsLibraryPanel (drag effects/transitions/titles)
  src/ui/deliver/           DeliverPage, render queue
  src/ui/dialogs/           ProjectManager, ProjectSettings, KeyboardCustomization, Preferences
  src/export/               Exporter (libavcodec encode H.264/AAC MP4, ProRes MOV)
  src/platform/             platform-specific helpers (Linux impl only)
  resources/                icons, qrc, default keymap json
  tests/<area>/test_*.cpp   each file → its own test exe (auto-globbed)
  packaging/linux|windows|macos
```
Namespace: everything in `namespace kk`.

### 5.1 Time conventions (MUST follow everywhere)
- Timeline positions/durations: `int64_t` **frames at the timeline frame rate** (`TimelineData::fps`).
- Source media position: `int64_t` **microseconds** (`sourceInUs`).
- Clip-local frame for keyframes: `frame - clip.start` (keyframes move with the clip).
- Source time at timeline frame f: `sourceInUs + llround((f - start) * 1e6 / fps * speed)`.
- Audio: 48 000 Hz, stereo, interleaved `float`; audio positions are sample indices at 48 kHz.

### 5.2 Core model (`src/core`) — value types + thin QObject wrappers
```cpp
struct Rational { int num = 25, den = 1; double toDouble() const; };
enum class Interp { Linear, EaseIn, EaseOut, EaseInOut, Hold };
struct Keyframe { int64_t frame; double value; Interp interp = Interp::Linear; };
struct AnimatedValue {           // a keyframeable scalar
  double base = 0; QVector<Keyframe> keys;   // sorted by frame
  double valueAt(int64_t localFrame) const;  // base if no keys
  void setKey(int64_t f, double v, Interp i = Interp::Linear); void removeKey(int64_t f);
  bool hasKeyAt(int64_t f) const; bool isAnimated() const;
};
struct ParamSet { QMap<QString, AnimatedValue> values; QMap<QString, QVariant> statics; }; // statics: colours, strings, bools, enums
struct EffectInstance { quint64 id; QString type; bool enabled = true; ParamSet params; };
struct TransitionData { QString type = "cross_dissolve"; int64_t duration = 25; ParamSet params; };
enum class ClipKind { Media, Title, Solid };
struct ClipData {
  quint64 id; ClipKind kind; QString mediaId; QString name; QColor label;
  int64_t start = 0, duration = 0, sourceInUs = 0; double speed = 1.0; bool enabled = true;
  quint64 linkGroup = 0;            // V+A from same source share a non-zero group
  ParamSet transform;               // keys: posX,posY,zoomX,zoomY,rotation,anchorX,anchorY,opacity,cropL,cropR,cropT,cropB; statics: flipH,flipV,zoomLinked
  ParamSet audio;                   // keys: volumeDb, pan
  ParamSet title;                   // Title clips: statics text,font,bold,italic,align,color,outlineColor,bgColor,bgEnabled,shadow; keys size,tracking,lineSpacing,outlineWidth,bgOpacity
  QColor solidColor;                // Solid clips
  QVector<EffectInstance> effects;  // applied in order
  std::optional<TransitionData> transitionIn, transitionOut;
  int64_t end() const { return start + duration; }
};
enum class TrackType { Video, Audio };
struct TrackData { quint64 id; TrackType type; QString name; bool enabled = true, locked = false,
                   muted = false, solo = false; int height = 0; std::vector<ClipData> clips; }; // clips sorted, never overlapping
struct Marker { int64_t frame; QString name; QColor color; };
struct TimelineData { QString name; Rational fps; int width = 1920, height = 1080;
  std::vector<TrackData> videoTracks, audioTracks;   // index 0 = V1/A1 (V1 is bottom-most)
  int64_t markIn = -1, markOut = -1; QVector<Marker> markers; int64_t duration() const; };
struct MediaItem { QString id; QString path; QString name; QString binId; media::MediaInfo info; };
struct Bin { QString id; QString name; QString parentId; };
```
- `Project` (QObject): owns `MediaPool`, `Timeline`, `QUndoStack`, project settings, file path,
  dirty flag; `save(path)`, `load(path)` (JSON), autosave.
- `Timeline` (QObject): `const TimelineData& data()`, `std::shared_ptr<const TimelineData> snapshot()`
  (immutable copy for render threads; cached until next change), signals `changed()`,
  `playheadChanged(int64_t)`; playhead lives here.
- **Editing protocol:** `project->beginEdit(tr("Move Clip"))` → mutate via `timeline->mutableData()`
  / `EditOps` → `project->endEdit()` → pushes one undo command with before/after snapshot.
  Interactive drags: begin on press, mutate live (cheap `changed()`), end on release.
  `cancelEdit()` restores.
- `EditOps` (pure functions on `TimelineData&`, unit-tested): insert, overwrite, placeOnTop,
  lift, rippleDelete, split(at frame, tracks), moveClips(delta frames, delta track),
  trimHead/trimTail (ripple and non-ripple), roll, slip, slide, closeGap, addTrack/removeTrack,
  find clip by id, linked-group helpers, snapping candidates.
- `SelectionModel` (QObject): selected clip ids, selected keyframes; signal `changed()`.
- `EffectRegistry`: descriptors `{id, name, category: VideoEffect|AudioEffect|Transition|Generator|Title,
  params: [{key, label, type: Double|Int|Bool|Color|Choice|String, min, max, def, step, unit, choices, animatable}]}`.
  The Inspector builds its UI generically from descriptors; the Compositor looks up implementations by id.

### 5.3 Media engine (`src/media`, namespace `kk::media`)
```cpp
struct MediaInfo { bool valid=false; QString error, path; bool hasVideo=false, hasAudio=false,
  isStillImage=false; int width=0, height=0, rotation=0; Rational frameRate; int64_t durationUs=0;
  int audioChannels=0, audioSampleRate=0; QString videoCodec, audioCodec, container; };
MediaInfo probe(const QString& path);                      // stills: hasVideo, isStillImage, durationUs=0
class VideoDecoder {   // NOT thread-safe; one instance per consumer/thread
  bool open(const QString& path, QString* err = nullptr); const MediaInfo& info() const;
  QImage frameAt(int64_t timeUs, QSize maxSize = {}); // ARGB32_Premultiplied, fits maxSize keeping aspect;
                                                       // fast sequential path; clamps to valid range
};
class AudioDecoder {   // NOT thread-safe
  bool open(const QString& path, QString* err = nullptr);
  int read(int64_t startSample48k, float* dst, int frames); // stereo interleaved, zero-fills past EOF
};
class MediaCache : public QObject {  // singleton, async (QThreadPool), disk cache in CacheLocation
  static MediaCache& instance();
  QImage thumbnail(const QString& path, int64_t timeUs, int height); // null → requested, signal later
  std::shared_ptr<const WaveformData> waveform(const QString& path); // nullptr → requested
signals: void thumbnailReady(QString path); void waveformReady(QString path);
};
struct WaveformData { int peaksPerSecond = 100; QVector<qint8> minL, maxL, minR, maxR; }; // scaled -127..127
```

### 5.4 Render (`src/render`)
- `Compositor::renderFrame(const TimelineData&, int64_t frame, QSize outSize, Quality) → QImage`:
  bottom track first; per clip: decode (own per-thread `VideoDecoder` pool keyed by path),
  crop → effects → transform (anchor, zoom, rotation, position; scaled from timeline resolution
  to `outSize`) → opacity → transitions → composite SourceOver. Titles via `QPainter` text
  (outline via `QPainterPath`, shadow, background box). Disabled tracks/clips skipped.
- Effects v0.1: Gaussian Blur, Brightness/Contrast, Saturation (simple), Sharpen, Mirror/Flip,
  Vignette, Mosaic, Drop Shadow (for titles/overlays).
- Transitions v0.1: Cross Dissolve, Dip to Color, Wipe (direction, softness), Push/Slide.
  Semantics: `transitionIn` on clip B with an adjacent left neighbour A = centred on the cut,
  using source handles (freeze last/first frame if no handles). Without neighbour = fade from
  transparent over the first `duration` frames. `transitionOut` likewise at the tail.
- Generators: Solid Color, Text title (inherit `ParamSet title`).
- `AudioMixer::mix(const TimelineData&, int64_t startSample, int frames, float* out)`: per clip
  `AudioDecoder`, volume (dB, keyframed, per-sample interpolation), pan, track mute/solo,
  audio cross-fade for transitions, soft clip.

### 5.5 Playback (`src/playback`)
`PlaybackEngine`: play/pause/stop, `setRate` (−8…8; J/K/L shuttle semantics: repeated L speeds up,
K pauses, K+J/K+L slow/frame step), loop in/out, audio master clock (`QAudioSink` pull mode) or
`QElapsedTimer` when silent/reverse; background render thread prefetching ~8 frames at viewer
resolution; drops frames rather than stalling audio; emits `frameReady(QImage, frame)`.
One engine for the program (timeline) and a light one for the source viewer (single clip).

### 5.6 Export (`src/export`)
`Exporter` (QThread): renders every frame of [in,out] (or whole timeline) with `Compositor` at full
res, mixes audio with `AudioMixer`, encodes via libavcodec. Presets: H.264 MP4 (libx264 CRF,
AAC 320k), H.265 MP4 (if libx265 exists), ProRes 422 HQ MOV (+PCM), PNG sequence? (later).
Settings: resolution, fps (= timeline), quality, filename/location, range. Progress + ETA + cancel.
Deliver page has a render queue. CLI: `kinoklip --render project.kkproj out.mp4`.

### 5.7 Default shortcuts (all remappable in Keyboard Customization, saved as JSON)
| Action | Key | Action | Key |
|---|---|---|---|
| Play/Pause | Space | Shuttle reverse / stop / forward | J / K / L |
| Prev / next frame | ← / → | Back / forward 1 s | Shift+← / Shift+→ |
| Prev / next edit point | ↑ / ↓ | Start / end of timeline | Home / End |
| Mark in / out | I / O | Clear in / out / both | Alt+I / Alt+O / Alt+X |
| Mark clip | X | Go to in / out | Shift+I / Shift+O |
| Selection / Trim / Blade tool | A / T / B | Split at playhead | Ctrl+B |
| Snapping | N | Linked selection | Ctrl+Shift+L |
| Delete (leave gap) | Backspace | Ripple delete | Delete / Shift+Backspace |
| Insert / Overwrite / Place on top | F9 / F10 / F12 | Add default transition | Ctrl+T |
| Undo / Redo | Ctrl+Z / Ctrl+Shift+Z | Cut / Copy / Paste | Ctrl+X / C / V |
| Select all / deselect | Ctrl+A / Ctrl+Shift+A | Nudge clip 1 frame | , / . |
| Zoom in / out / fit | Ctrl+= / Ctrl+- / Shift+Z | Add marker | M |
| Add keyframe (inspector focus) | Alt+K? (TBD) | Prev / next keyframe | [ / ] |
| Import media | Ctrl+I | Save / Save as | Ctrl+S / Ctrl+Shift+S |
| New / open project | Ctrl+N / Ctrl+O | Media / Edit / Deliver page | Shift+2 / Shift+4 / Shift+8 |
| Full-screen viewer | Ctrl+F | Enable/disable clip | D |


### 5.8 Supported formats (import and export)
Acceptance is probe-based (anything FFmpeg/Qt/libheif/LibRaw can open is accepted); the extension
lists below only drive file dialogs and the media browser filter. One registry:
`media/SupportedFormats.h` (categories, extensions, `isLikelyMedia(path)`).

**Import**
- Video containers: mp4, m4v, mov, mkv, webm, avi, mts/m2ts/ts, mxf, flv, wmv/asf, 3gp, ogv, mpg/mpeg/vob, dv, y4m.
- Video codecs: H.264, H.265/HEVC, AV1, VP8, VP9, ProRes, DNxHD/DNxHR, CineForm, MPEG-2/4, MJPEG, DV, WMV, Theora, FFV1, raw/uncompressed.
- Audio: wav, aiff/aif, flac, mp3, aac/m4a, ogg/oga (Vorbis), opus, wma, ac3/eac3, dts, alac, caf, w64, mka.
- Images (stills): png, jpg/jpeg, webp, bmp, gif (first frame; animated gif as video), tiff/tif, tga, svg,
  ico, heic/heif and avif (libheif), jxl (FFmpeg libjxl), exr, dpx, psd (flattened), qoi, pcx, sgi,
  camera RAW: cr2/cr3/nef/arw/dng/raf/orf/rw2 (LibRaw, half-size fast decode).
- Image sequences: numbered files (name_0001.png ...) detected on import and offered as one clip.

**Export (Deliver presets)**
- MP4: H.264 (libx264 / NVENC / VAAPI), H.265 (libx265 / NVENC / VAAPI), AV1 (SVT-AV1 / NVENC / VAAPI where supported) + AAC.
- MOV: ProRes 422 Proxy/LT/422/HQ, ProRes 4444 (with alpha), DNxHR LB/SQ/HQ/HQX + PCM.
- MKV: any of the above video codecs + FLAC/Opus/AAC.
- WebM: VP9 or AV1 + Opus.
- Audio only: WAV (PCM 16/24/32f), FLAC, MP3, AAC (m4a), Opus.
- Stills / sequences: PNG, JPEG, TIFF, EXR sequence; single frame grab (PNG/JPEG).
- GIF (palettegen) for short clips.

---

## 6. Execution plan (agents, git branches)

Repo: `~/cutwise` (git). Integration branch `main` must **always build**. Each agent works in its
own `git worktree` (`~/cutwise-wt/<name>`, branch `<name>`), owns a set of directories, commits
there, and I (orchestrator) merge. Max **4 agents at a time** to stay inside the Max-plan
usage window (resets 15:00). An AppImage is produced after every phase so there is always
something runnable even if the budget runs out.

| Phase | Agent | Owns | Depends on |
|---|---|---|---|
| 0 | me | skeleton: CMake, dirs, PLAN, main.cpp stub | — |
| 1 | **foundation** | `src/core`, `src/app`, `src/ui/common`, shell of all pages/panels (placeholders), theme, icons, menu/actions/shortcut manager, project save/load, tests/core | skeleton |
| 1 | **media** | `src/media`, tests/media, test fixtures script | skeleton |
| 2 | **timeline** | `src/ui/timeline`, EditOps additions | foundation |
| 2 | **render** | `src/render` (+ CLI `--render-frame`) | foundation, media |
| 2 | **panels** | `src/ui/mediapool`, `src/ui/inspector`, `src/ui/effects`, `src/ui/dialogs` | foundation, media |
| 3 | **playback** | `src/playback`, `src/ui/viewer` | render |
| 3 | **export** | `src/export`, `src/ui/deliver`, CLI `--render` | render |
| 3 | **formats** | `media/SupportedFormats`, still-image loaders (Qt, FFmpeg, libheif, LibRaw), image sequences, tests per format | media |
| 3 | **theme** | Breeze look: Theme emulation, palette-derived editor colours, Breeze icon subset via fromTheme | foundation |
| 3 | **packaging** | `packaging/`, AppImage script, .desktop, icon | foundation |
| 4 | **qa** (1–2) | end-to-end scenarios, bug fixes, polish | all |
| 4 | me | final merge, AppImage, morning-report.md | all |

Rules for every agent:
0. **Read `docs/coding-style.md` and follow it strictly** (naming, doc comments on every public
   class/function, file headers, module readme.md with a "How to extend" section,
   `scripts/format.sh` before each commit). Readability and extensibility are hard requirements.
1. Read this plan.md first; follow §5 contracts. If a contract must change, make the minimal
   change, document it in the commit message and in the final report.
2. Only edit files in owned directories (+ registration hooks explicitly allowed in the brief).
3. Qt 6.4 API only. C++20. No new third-party deps without asking the orchestrator.
4. Build with `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build`
   (each worktree has its own `build/`). Zero warnings policy for new code where reasonable.
5. Write tests for logic; run `ctest --test-dir build --output-on-failure`.
6. Commit early and often on your branch; finish with everything committed and a short report:
   what's done, what isn't, any contract changes.

## 7. Testing strategy
- Unit: keyframe interpolation, EditOps, undo snapshots, serialization round-trip, decoder seek
  accuracy (ffmpeg `testsrc2` fixtures with burnt-in frame numbers), audio sample accuracy.
- Render golden checks: solid + title + transform produce expected pixels.
- UI smoke (offscreen): open each page, simulate import/drag/split/undo via code, `--screenshot`.
- End-to-end: build a project programmatically → `--render` → `ffprobe` duration/streams check.
- Manual-ish: screenshots reviewed by the orchestrator for layout sanity.

## 8. Legal / licensing notes (not legal advice)
- Clean-room: only public, general knowledge of how NLEs work. No Resolve binaries, decompiling,
  assets, icons, docs text, names or trademarks. Generic layout & common shortcuts are industry-wide.
- Choose your own product name before publishing; check it isn't a trademark.
- **Licensing matters more than Resolve here:** the AppImage bundles Qt (LGPLv3) and FFmpeg built
  with libx264 (GPL). Distributing that bundle means **the app must be GPL-compatible (GPLv3
  suggested)**. If you ever want closed-source/commercial, swap to an LGPL FFmpeg build without x264
  (e.g. use OpenH264 or hardware encoders). A `LICENSE` choice is left for you; code headers say
  "SPDX-License-Identifier: GPL-3.0-or-later" by default.

## 9. Later (after v0.1)
Color page (wheels, curves, LUTs, scopes) → GPU pipeline (OpenGL/Vulkan) → proxies & render cache →
speed ramps/retime → multicam → Fairlight-like mixer → Windows (MSVC + windeployqt + NSIS/MSIX) →
macOS (universal build, macdeployqt, notarization).
