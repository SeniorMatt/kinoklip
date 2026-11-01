# `src/export` — export engine

Renders a timeline snapshot to files: movies, audio-only files, image sequences, single
frames and GIFs (plan.md §5.6, §5.8). Namespace `kk` (public API) and `kk::exporter`
(internal helpers), logging category `kk.export`. Depends on `core`, `media` (probe, one
log-routing hook), `render` (Compositor, AudioMixer) and FFmpeg; never on widgets.

## Public API

| Header | What |
|---|---|
| `ExportSettings.h` | `ExportSettings` (container, `VideoExportSettings`, `AudioExportSettings`, range, output path), `EncoderBackend`, `RateControl`, `RangeMode`; `validate()`, `normalize()`, JSON form, `outputSize()`, `frameRange()`, `sequencePattern()` |
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
