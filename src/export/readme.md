# `src/export` — export engine

Renders a timeline snapshot to files: movies, audio-only files, image sequences, single
frames and GIFs (plan.md §5.6, §5.8). Namespace `kk` (public API) and `kk::exporter`
(internal helpers), logging category `kk.export`. Depends on `core`, `media` (probe, one
log-routing hook), `render` (Compositor, AudioMixer) and FFmpeg; never on widgets.

## Public API

| Header | What |
|---|---|
| `ExportSettings.h` | `ExportSettings` (container, `VideoExportSettings` incl. key frame interval and frame reordering, `AudioExportSettings`, range, output path), `EncoderBackend`, `RateControl`, `RangeMode`; `validate()`, `normalize()`, JSON form, `outputSize()`, `frameRange()`, `sequencePattern()` |
| `ExportFormats.h` | the format matrix as data: `exportContainers()`, `exportVideoCodecs()`, `exportAudioCodecs()` |
| `ExportPresets.h` | built-in presets (`resources/export/presets.json`) + user presets (`<AppData>/export_presets.json`) |
| `EncoderCatalog.h` | encoder candidates per codec, run-time hardware detection (NVENC, VAAPI, QSV) |
| `ExportJob.h` | one render job: blocking `run(progress)`, `cancel()`, `ExportResult` |
| `RenderQueue.h` | GUI-side sequential background queue (Deliver page) |
| `RenderCommand.h` | `kinoklip --render <project.kkproj> <out-file> [--preset id]` |

```cpp
kk::ExportSettings s;
kk::ExportPresets().apply("youtube_1080p", s);       // or fill the fields yourself
s.outputPath = "/home/me/Videos/cut.mp4";
kk::Compositor compositor;                           // GUI thread
kk::ExportJob job(s, project->timeline()->snapshot(),
                  kk::makeMediaPathMap(project->mediaPool()->state()));
job.setCompositor(&compositor);
kk::ExportResult r = job.run([](const kk::ExportProgress& p) { /* frames, fps, ETA */ });
```

## Pipeline

```
 job thread (ExportJob::run)                         Compositor render thread
 ───────────────────────────                         ────────────────────────
 keep 3 renderAsync(OutputImage) queued ───────────► render frame N+2 (GL) → PBO readback
 future(N).get() ◄─────────────────────────────────── QImage ARGB32 premultiplied
   │
   ├─ VideoEncoder: RGB32 ──swscale (slice threads)──► YUV 4:2:0/4:2:2/4:4:4, RGB, 10-bit
   │                 (EXR: own sRGB→linear float loop; GIF: palettegen/paletteuse graph;
   │                  VAAPI: NV12 → av_hwframe_transfer_data → VA surface)
   │                 libavcodec encoder ──► packets (pts = frame index, tb 1/fps)
   ├─ AudioMixer.mix(48 kHz) up to the end of frame N ─► AudioEncoder: swresample →
   │                 AVAudioFifo (codec frame size) → libavcodec (pts = sample index)
   └─ MediaWriter: av_interleaved_write_frame (dts-ordered A/V interleaving),
                   movflags=+faststart for mp4/mov/m4a, image2 pattern for sequences
```

* **Timestamps and sync**: the video stream's time base is exactly 1/fps of the timeline
  (no retiming); frame k of the range has pts k. Audio pts counts output samples from the
  same instant, and after each video frame the mixer is advanced to
  `framesToSamples48k(first + k + 1) - framesToSamples48k(first)`, so audio never drifts.
  Encoder priming (AAC, Opus) is written by the muxer as an edit list / pre-skip, which
  decoders honour — `test_export avSync` checks a flash frame against an audio impulse in
  MP4/AAC (offset 0.3 ms), MOV/PCM and MKV/FLAC (0 ms) and WebM/Opus (0 ms with FFmpeg 9;
  6.5 ms = the Opus pre-skip with FFmpeg 6.1's decoder; well inside the one-frame tolerance).
* **Colour**: YUV outputs are tagged and converted as BT.709 limited range (JPEG: BT.601 full
  range, the JFIF convention); RGB outputs are sRGB; EXR is linear (sRGB decoding curve),
  premultiplied. Alpha outputs (ProRes 4444, PNG/TIFF/EXR with alpha) are un-premultiplied
  first; opaque outputs use the premultiplied RGB directly (= the picture over black).
* **RGB → YUV on the CPU, not the GPU**: the Compositor API hands out RGBA textures/images
  only, so a GPU conversion would need a second readback path in `src/render` (not owned
  here). swscale's RGB → YUV path turned out slow (6-10 ms per 1080p frame idle, 22-26 ms on
  the loaded dev box), so the 8-bit 4:2:0 hot path uses an own fixed-point BT.709 converter
  (`RgbToYuv.h`, checked against swscale in `fastYuvMatchesSwscale`) split into row bands on
  a small thread pool: 2.3-3.7 ms per 1080p frame under the same load. A GPU NV12 pass +
  readback is the next step if hardware encoders become conversion-bound (see TODO).
* **Errors and cancel**: every failure after the header was written goes through one path
  that aborts the muxer and deletes the file (or every file of a sequence). `cancel()` is an
  atomic flag checked once per frame; the partial output is removed the same way.
* **Hardware encoders**: `EncoderCatalog::candidates(codec, preference)` returns working
  hardware encoders (Auto order NVENC → VAAPI → QSV) followed by software. Detection opens
  each encoder once with a 320x240 test frame (VAAPI on every `/dev/dri/renderD*`, normal then
  low-power entry point) and caches the result for the process lifetime. If the chosen
  encoder cannot open at the real size, the job falls back to the next candidate and reports
  the encoder actually used in `ExportResult::videoEncoder`. `KINOKLIP_HWENC=off` disables
  hardware encoding.
* **Quiet probing**: NVENC is not tried without `/dev/nvidiactl`, QSV not without an Intel
  render node and a oneVPL/MFX runtime (`media::nvidiaDriverPresent()`,
  `media::qsvRuntimePresent()`). FFmpeg's messages during the remaining probes are captured
  (`FfmpegQuiet`; the first error becomes the status reason), per-encoder results are debug
  output, and one info line summarises:
  `kk.export: hardware encoders: VAAPI h264/hevc (iHD 24.1.0, /dev/dri/renderD128), NVENC
  unavailable (no NVIDIA driver), QSV unavailable (no Intel GPU with a oneVPL/MFX runtime)`.
  `tests/export/test_encoderprobe` checks that probing logs no warning at all.

## Threading

* `ExportJob::run()` blocks; `RenderQueue` runs it on a `QThread` (low priority) and
  forwards progress to the GUI thread at most every 100 ms. The CLI runs it on the main thread.
* The queue owns its own `Compositor` (separate from the Deliver page's preview compositor, so
  the preview never waits behind export frames). The Compositor is created on the GUI thread.
* Jobs keep immutable `TimelineData` / `MediaPathMap` snapshots taken when queued: editing
  continues while a job renders and does not affect it.
* `EncoderCatalog` is thread-safe (one mutex; detection is serialised).
* Encoders, writer, mixer and decoders belong to the job thread only.

## Format matrix (export side, plan.md §5.8)

| Container | Video | Audio | Tested by `test_export` |
|---|---|---|---|
| MP4 | H.264, H.265, AV1 (hw or sw) | AAC, MP3, Opus | YouTube 1080p/4K, Vimeo, H.265 MP4, AV1 MP4 presets; hw encoders |
| QuickTime | ProRes Proxy/LT/422/HQ/4444/4444 XQ (+alpha), DNxHR LB/SQ/HQ/HQX, H.264, H.265 | PCM 16/24, AAC | ProRes HQ, ProRes 4444 alpha, DNxHR HQ |
| Matroska | H.264, H.265, AV1, VP9, ProRes, DNxHR | FLAC (16/24), Opus, AAC, PCM | MKV H.264 + FLAC |
| WebM | VP9, AV1 | Opus | Web WebM |
| Audio only | — | WAV (PCM 16/24/32f), FLAC, MP3, AAC (m4a), Opus | every audio preset |
| Image sequences | PNG, JPEG, TIFF (deflate), OpenEXR (half, zip16) | — | every sequence preset (re-imported as a sequence) |
| Single frame | PNG, JPEG (via QImage) | — | frame grab presets |
| GIF | GIF (palettegen stats over all frames + sierra2_4a dithering) | — | GIF preset |

Software encoders: libx264, libx265, libsvtav1 (libaom-av1 fallback), libvpx-vp9, prores_ks,
dnxhd, png, mjpeg, tiff, exr, gif. Hardware: h264/hevc/av1 `_nvenc`, `_vaapi`, `_qsv`;
vp9 `_vaapi`, `_qsv`.

## Proxy media, optimized media, render cache

| File | What |
|---|---|
| `ProxyMedia.h` | intermediate formats (`proxyFormats()`, `renderCacheFormats()`), `intermediateSettings()`, `makeProxyJob()` (one media item -> one-clip timeline -> QuickTime file) |
| `ProxyQueue.h` | background proxy / optimized media jobs (private RenderQueue), progress per item, cancel, atomic rename + sidecar + link on success, `deleteLinked()` |
| `RenderCacheManager.h` | Playback > Render Cache: keys of the active timeline, idle-time background rendering of wanted chunks, pause during playback, eviction, `RenderCacheView` for playback, ruler ranges |

**Proxies vs optimized media** (Resolve's two concepts, same machinery here):
* *Proxy media*: small, fast-to-decode copies (default H.264 fast decode at "Choose
  automatically" = at most 540 lines) in a portable folder of the user's choice (Preferences >
  Media Storage), linkable by hand, used according to Playback > Proxy Handling, ignored by
  export unless Deliver's "Use proxy media" is on. For editing on weak laptops.
* *Optimized media*: edit-friendly intermediates (default DNxHR LB at original resolution,
  intra-only, so seeking and scrubbing are instant) that Kinoklip manages in the cache folder;
  used whenever "Use Optimized Media If Available" is on and no proxy is used. For sources that
  are slow to decode at full quality (long-GOP HEVC, RAW-ish formats).

Both are *display-equivalent*: rendered by the compositor from a one-clip timeline the size of
the proxy, i.e. upright, square pixels, same display aspect, frame k at k / fps of the source
rate, the source's sound as PCM 16-bit. The renderer decodes them with the original's geometry
(`ProxyRef::displaySize`), so crop values, fitting and rotation behave exactly as with the
original. Stale detection: `MediaLink::source` = size + mtime of the original when the file was
made (core/MediaLinks.h).

**Choice of the default proxy codec** (measured on the dev box, FFmpeg 6.1 software decoding,
one thread, 1080p60 noisy test pattern transcoded, 240 frames; ms per frame):

| Codec (960x540) | decode | size / 4 s | | Codec (1920x1080) | decode |
|---|---|---|---|---|---|
| H.264 GOP 12, no B, tune fastdecode | **1.5** | 2.4 MB | | H.264 GOP 12 fastdecode | 10.8 |
| H.264 GOP 12, no B, CABAC | 2.9 | 2.0 MB | | H.264 CABAC (= camera-like) | 21.1 |
| H.264 intra only, fastdecode | 3.2 | 6.5 MB | | DNxHR LB | 8.7 |
| MJPEG q4 | 2.1 | 7.6 MB | | MJPEG q4 | 8.7 |
| DNxHR LB | 2.2 | 11.8 MB | | ProRes 422 Proxy | 11.6 |
| ProRes 422 Proxy (10-bit) | 3.5 | 15.1 MB | | the original (H.264 High, long GOP) | 21.0 |

H.264 fast decode wins at proxy sizes (decode cost and size), seeks decode at most 11 extra
frames (~16 ms at 540p), and every laptop GPU (Iris Xe: VAAPI) decodes it in hardware. DNxHR
LB is the default for optimized media (intra) and the render cache: at 1080p it decodes in
8.7 ms on one core, 5 ms with 4 slice threads (DNxHR SQ: 21.7 / 12 ms, closer to the uncached
picture; selectable).

**Render cache design**: the active timeline is cut into one-second chunks; a chunk's key is a
SHA-1 over the cache version, app version, format, timeline format, range and the JSON of every
clip that can influence the chunk (+ media fingerprints, nested timelines); see
`render/RenderCacheKeys.h` for the exact rules. The key is the file name
(`<cache folder>/RenderCache/<key>.mov`), so invalidation needs no bookkeeping: edits change
keys, unchanged chunks keep theirs (also across undo, timelines and projects). Wanted chunks
(Smart / User rules) are rendered after the idle delay through the normal export pipeline
from the camera originals; the Compositor substitutes a cached chunk only for the exact
snapshot (pointer) the view was built for. Cached pictures are opaque (rendered over black).

## Performance

`build/test_exportperf` (1080p25 H.264 CRF/CQ 20, 100 frames of the 1080p fixture + AAC).
"Encode only" = conversion + encoder on pre-rendered frames; "full pipeline" = compositor
(here the GPU path: the NVIDIA EGL context works even on the offscreen platform), decoding,
conversion, encoding, audio and muxing. Dev box i7 Comet Lake 4C/8T + GTX 1650, measured
while other builds kept the load average at 17-21 (numbers on an idle machine are higher):

| Encoder | encode only | conversion | full pipeline |
|---|---|---|---|
| libx264 (medium) | 108 fps | 3.7 ms/frame | 28 fps |
| h264_nvenc (p5) | 296 fps | 2.3 ms/frame | 68 fps |
| before the own converter (swscale) | 25 / 42 fps | 26 / 22 ms/frame | 10 / 24 fps |

Arch container (FFmpeg 9, no GPU → QPainter compositor): libx264 65.8 fps encode only,
20.5 fps full pipeline.

VAAPI (the target laptop's path) could not be measured: the render nodes belong to the other
logged-in user's seat on the dev box; `test_exportperf` and `test_export hardwareEncoders`
pick it up automatically where `/dev/dri/renderD*` is accessible.

## How to extend

### A new preset
Add an object to `resources/export/presets.json` (`id`, `name`, `category` = Web / Master /
Audio / Images, `description`, `settings` in the `ExportSettings::toJson()` form; omitted keys
keep the defaults). `test_export presetsLoad` validates it and `exportPreset` renders it
automatically — nothing else to change.

### A new video codec
1. Add a row in `makeVideoCodecs()` (`ExportFormats.cpp`): quality range, profiles, alpha.
2. List it in the containers that may carry it (`makeContainers()`).
3. `EncoderCatalog.cpp`: its software encoder(s) in `softwareNames()`, hardware encoders in
   `kHardware` (Auto order).
4. `VideoEncoder.cpp`: preferred pixel formats in `preferredFormats()`, encoder options in
   `configureEncoder()` (speed preset, quality / bitrate mapping, profile).
5. Add a preset if users should see it; the preset test covers it.

### A new audio codec
Row in `makeAudioCodecs()` (encoder name, lossless, sample rates), add it to the containers;
`AudioEncoder` picks the sample format from the encoder's supported list (prefers float).

### A new container
Row in `makeContainers()` with the libavformat muxer name, extension, allowed codecs and
`faststart`. Muxer options beyond faststart / image2 `start_number` / GIF loop go into
`MediaWriter::writeHeader()`.

### A new hardware backend
Add the `EncoderBackend` value (`ExportSettings.h`, id/name functions), rows in `kHardware`,
device handling in `probe()` (`EncoderCatalog.cpp`) and in `VideoEncoder::open()` if the
encoder needs hardware frames (as VAAPI does).

## Known limitations / TODO

* TODO(export): GPU RGB → NV12 pass in the compositor and zero-copy VAAPI upload
  (dma-buf/EGLImage) — today frames go GPU → CPU (PBO) → swscale → (VAAPI) upload.
* TODO(export): GIF keeps every frame in memory until the palette is known (palettegen emits
  at EOF): ~1 MB per 480x270 frame; keep GIFs short.
* TODO(export): no two-pass encoding, no HDR (PQ/HLG) tagging, no 10-bit H.265/AV1 delivery
  (ProRes/DNxHR HQX are 10-bit), no multichannel audio (stereo only, like the mixer).
* QSV is detected and used when it works, but untested on the dev box (no Intel media driver
  access for this user); on Intel laptops VAAPI is the preferred path anyway.
* Custom output sizes with a different aspect ratio stretch the picture (the compositor
  scales the timeline frame to the output size); use the timeline aspect.
* Image sequences are numbered from 0 (`name_00000.png`), whatever the range start.
