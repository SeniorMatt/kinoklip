# `src/media` — media engine

Everything that touches media files: probing, frame-exact video decoding (software and GPU),
sample-exact audio decoding, still images, image sequences, the supported-format registry, and
cached UI previews (thumbnails, poster frames, waveforms).
Namespace `kk::media`, logging category `kk.media`. Depends only on `core` (for `Rational`), `pack` (embedded media, QtCore only),
QtCore/QtGui, FFmpeg and — optionally — libheif and LibRaw; never on Qt Widgets.

## Conventions (read this first)

| Quantity | Unit | Notes |
|---|---|---|
| media time | `int64_t` **microseconds** | 0 = start of the file (`MediaInfo::startTimeUs` is the container timestamp of 0). Audio and video of one file share this origin, so they stay in sync. |
| audio position | `int64_t` **sample frames at 48 kHz** | `sample = llround(timeUs * 48000 / 1e6)` |
| audio format | stereo, interleaved `float`, 48 kHz | always, whatever the file contains |
| rotation | degrees **clockwise** | `MediaInfo::rotation`, `VideoFrame::rotation` |

A video request at time *t* returns the frame whose presentation interval `[pts, next pts)`
contains *t* (exact for VFR too); requests before the first / after the last frame are clamped.

## Key classes

| File | What |
|---|---|
| `MediaSource.h` | Where a media path's bytes are: a file, or an entry inside a single-file project (`film.kinoklip/media/<id>/clip.mp4`, see `src/pack`). Every open, stat and cache identity in this module goes through it: FFmpeg reads entries through a custom `AVIOContext` (own handle, pread), Qt image readers through a `RangeDevice`, libheif / LibRaw from memory. `mediaExists(path)` replaces `QFileInfo::isFile()`. |
| `MediaInfo.h` | `MediaInfo` struct + `probe(path)` (synchronous, any thread). Stills report `isStillImage`, `durationUs = 0`. |
| `VideoDecoder.h` | `decodeFrame(timeUs)` → `shared_ptr<const VideoFrame>` (raw planes, GPU compositor path); `frameAt(timeUs, maxSize)` → `QImage` ARGB32_Premultiplied (thumbnails, tests). `Accuracy::{Exact, NearestKeyframe}`. Not thread-safe; one per thread. |
| `VideoFrame.h` | Raw frame: `PixelFormat` (NV12, P010, YUV420P/422P/444P, 10-bit planar, BGRA premultiplied), plane pointers/strides, colour matrix/range/transfer, pts, duration, SAR, rotation. Immutable, refcounted, may outlive the decoder. |
| `HwAccel.h` | `HwAccelMode {Auto, Off, Cuda, Vaapi, Vulkan}`, `setPreferredHwAccel()`, `hwCapabilities()` (for Preferences), `probedHwDevices()` (the backends already tried, opens nothing; for Help > Report a Bug). |
| `WavWriter.h` | streams interleaved float samples into a 16/24-bit PCM WAV file, header patched on close (voice-over takes) |
| `AudioDecoder.h` | `read(startSample48k, dst, frames)`: sample-accurate, zero-filled outside the media. Not thread-safe. |
| `MediaCache.h` | Singleton: `thumbnail()`, `posterFrame()`, `waveform()` — return cached result or null + async job + signal. Memory LRU + disk cache. |
| `WaveformData.h` | per 5 ms block and channel: min/max and RMS, linear (0 dBFS = ±32767, never normalised), plus a 4x multi-resolution summary for per-pixel-column `range()` queries; compute/save/load (format version 2). |
| `SupportedFormats.h` | The registry of import extensions per `MediaCategory` (Video, Audio, Image, RawImage, ImageSequence); `dialogFilter()` for QFileDialog, `categoryFor()`, `isLikelyMedia()`. UI-only: acceptance is decided by `probe()`. |
| `ImageLoader.h` | Still images → ARGB32 premultiplied, upright: backend by extension (Qt, libheif, FFmpeg, LibRaw) with fall-back; `loadCached()` = process-wide LRU still cache (256 MiB); RAW half/full size. |
| `FolderScan.h` | `scanMediaFolder()` for Import Media Folder: importable files per folder in natural order (`naturalCompare()`), numbered stills folded into sequence paths, hidden / junk files skipped (`isHiddenOrJunkName()`), symlink loops cut by canonical path; re-entrant, with a cancel callback |
| `ImageSequence.h` | Sequence path convention (`/dir/name_%04d.png#fps=24`), `detectSequence(file)` for "import as sequence", scans with gap reporting. |
| internal (formats) | `ImageBackends.h` + `*ImageBackend.cpp` (one file per library), `ImageFrameList` (sequences and Qt animations as frame-indexed sources; `routeFor()` = the routing decision shared by `probe()` and `VideoDecoder::open()`). |
| internal | `FFmpegUtil` (RAII, open/seek helpers, log routing + `FfmpegLogCapture`), `HwDevices`/`HwAccel.cpp` (backend table, shared device contexts), `VideoFrameFactory` (hw download + pooling), `FrameConverter` (swscale → QImage), `HdrToSdr` (HLG / PQ / BT.2020 → SDR BT.709, CPU and GLSL). |

## Data flow

```
                    ┌──────────── VideoDecoder ────────────────────────────────────┐
file ─ libavformat ─┤ seek / decode-forward state machine (cur + look-ahead frame) │
                    │   libavcodec (SW 4 threads, more above 1080p, or VAAPI/CUDA/Vulkan hwaccel)   │
                    │      │ only the frame actually returned is materialised:    │
                    │      ├─ decodeFrame(): hw surface → av_hwframe_transfer_data │
                    │      │    → pooled NV12/P010 │ SW frame shared by reference  │
                    │      └─ frameAt(): swscale (colour matrix/range, scaling)    │
                    │                    → QImage ARGB32_Premultiplied + rotation  │
                    └──────────────────────────────────────────────────────────────┘
file ─ libavformat ─ AudioDecoder ─ libavcodec ─ swresample (48k stereo float) ─ read()
MediaCache: QThreadPool jobs → (disk cache | VideoDecoder/AudioDecoder) → memory LRU → signal
```

### Routing (probe() and VideoDecoder::open())

```
path ── "…%04d…[#fps=]" ─────────────────────────► ImageFrameList (sequence: one ImageLoader::load per frame)
     ── image extension ─ ImageLoader::readHeader ┬ single image ──► ImageLoader::loadCached (still)
     │                                            ├ animated WebP ─► ImageFrameList (QImageReader, sequential)
     │                                            └ animated GIF / unreadable header ─┐
     └─ anything else ──────────────────────────────────────────────────────────────┴► FFmpeg
```
Stills keep `VideoDecoder`'s existing behaviour for callers: `frameAt()` returns the (cached,
scaled-on-demand) image for any time, `decodeFrame()` a `Bgra` VideoFrame sharing its pixels.
Sequences and animations return the frame whose interval contains the time, like video.

### Image sequences

* Path: `<dir>/<prefix>%0Nd<suffix>[#fps=num[/den]]` (`%d` unpadded, `%%` literal percent).
  Frame k shows number `first + k` where `first` is the lowest number on disk; a missing number
  repeats the previous file. Default 25 fps. `ImageSequence::makePath(pattern, fps)` builds it.
* `probe()` sets `isImageSequence`, `frameRate`, `durationUs = frames / fps`,
  `sequenceFirstNumber/LastNumber/MissingFrames` (and a note in `error` when frames are missing).
* Importer: `detectSequence(clickedFile)` → `SequenceInfo{found, pattern, first, last, missing}`;
  offer "import as sequence", then import `ImageSequence::makePath(info.pattern, chosenFps)`.
  `ImageSequence::displayName(path)` gives "render_[0001-0240].exr" for lists.
* Sequence frames are decoded on demand (not via the still cache, which they would flush).
  MediaCache has no disk cache for sequence paths (no single file to fingerprint).

### Seeking (VideoDecoder)

The decoder keeps the last returned frame (`cur`) and the following one (`next`).
* Same frame again → cached result (no work).
* Target ahead, and the keyframe index says no keyframe lies between us and the target (or,
  without a trustworthy index, the target is < 2 s ahead) → decode forward, no seek.
* Otherwise `av_seek_frame(BACKWARD)` to the keyframe at/before the target and decode forward.
  If the first frame after a seek is *after* the target (B-frame reordering, MPEG-TS seeking by
  DTS) or nothing is decodable there (truncated file), retry 1 s, 2 s, 4 s earlier, then from the
  very start. Frames skipped on the way are recycled without conversion or GPU download.

### Hardware decoding

* One table (`kBackends` in `HwAccel.cpp`) lists the backends and the **Auto order**. Linux:
  **VAAPI** (render node of the GPU that drives the display, then Intel/AMD, then others) →
  **CUDA/NVDEC** → **Vulkan Video** (first physical device that enables
  `VK_KHR_video_decode_queue`) → software.
* `HwDeviceManager` creates one `AVHWDeviceContext` per backend on first use and shares it
  (refcounted `AVBufferRef`) between all decoders; failures are cached.
* At `open()` the first frame is decoded **and downloaded** to validate the whole path. Falls
  back to software when: FFmpeg has no hwaccel for the codec (ProRes, PNG...), the GPU rejects
  the profile (e.g. H.264 4:4:4), decoding errors, or a failed download — at open or mid-stream.
  `VideoDecoder::activeHwAccel()` tells what is actually used.
* Hardware frames are downloaded only for frames that are returned, into a buffer pool:
  8-bit → **NV12**, 10-bit → **P010** (NVDEC's P016 is reported as P010: same layout).
* VAAPI downloads map the surface (`av_hwframe_map`) and copy with `av_image_copy_uc_from()`
  (SSE4.1 streaming loads): the mapping is write-combining memory, which plain loads (what
  `av_hwframe_transfer_data` uses) read at a few hundred MB/s — that made VAAPI
  "download-bound" (86 fps for 1080p below). Falls back to `av_hwframe_transfer_data` if
  mapping fails; `KINOKLIP_VAAPI_COPY=transfer` forces the old path (diagnostics).
  TODO(media): re-measure on Intel (the numbers below predate this change).
* H.264 on CUDA/Vulkan: FFmpeg's flush re-creates the whole hw decoder (~250 ms). When the
  packet at the new position is an IDR we skip the flush ("soft seek") and drop stale frames
  via packet→frame `opaque` generation tags. VAAPI re-inits in ~5 ms and always flushes.
* Global preference: `setPreferredHwAccel()`; per decoder: `VideoDecoder::setHwAccel()`;
  for debugging without touching settings: environment `KINOKLIP_HWACCEL=off|auto|vaapi|cuda|vulkan`.
* Linux permissions: VAAPI/Vulkan need access to `/dev/dri/renderD*` (member of group
  `render`, or the active seat user). Without it they are reported unavailable.
  CUDA is not tried at all without `/dev/nvidiactl` (no `CUDA_ERROR_NO_DEVICE` noise).

### Hardware decoding robustness (alpha.3: "Failed to sync surface" on Iris Xe)

* **One device, many threads.** All decoders share one `AVHWDeviceContext` per backend (one
  VADisplay), used at the same time by the render thread, its decode-ahead and parallel-decode
  workers, the thumbnail pool and export. FFmpeg's `hwcontext_vaapi` takes no lock and libva
  promises no thread safety, so every hardware decoder holds the device's call lock
  (`HwDeviceManager::callLock()`) around each libavcodec call that can reach the driver
  (`avcodec_send_packet/receive_frame/flush_buffers`, open, free incl. the surface pool) and
  around each download. Per call, not per frame: GPU decoding stays asynchronous, only the
  download's `vaSyncSurface` waits. Default: VAAPI only; `KINOKLIP_HW_SERIALIZE=off|vaapi|all`.
* **Surface lifetime.** The decoder references at most `cur`, `next` and the frame being
  received (3) on top of the DPB; `extra_hw_frames = 4` covers that. A download works on `cur`,
  which the decoder owns, and `av_hwframe_map()` itself references the source frame until the
  mapping is freed, so a surface can never be recycled while it is read.
* **Download contract.** `av_hwframe_map(dst, hw, AV_HWFRAME_MAP_READ)` with `dst->format` =
  the frames' `sw_format`: FFmpeg calls `vaSyncSurface()` first, then (READ without DIRECT)
  copies into a VAImage and maps it; we copy with streaming loads. An `EIO` from mapping (the
  sync failed) is not retried through `av_hwframe_transfer_data()` (same sync, a second error
  line) and does not disable the fast path; other mapping errors fall back to transfer.
* **Failures are recoverable.** A failed download is retried once; if it fails again the
  decoder re-opens in software and decodes the *same* frame (no lost frame, never a garbage
  picture). FFmpeg's messages during downloads are captured (`FfmpegLogCapture`) and become
  the reason of one warning per file:
  `kk.media: VAAPI decoding failed for a.mp4 at 1.240 s (frame download: Input/output error
  (AVHWFramesContext: Failed to sync surface 0x3: 1 (operation failed))); continuing in
  software (4 threads)`. After 3 such fall-backs the backend is disabled for the session
  (one warning). A profile the GPU lacks is not counted.
* **Log lines.** Per file, once: `kk.media: decoding a.mp4 (h264 high 1920x1080) with VAAPI
  (iHD 24.1.0, /dev/dri/renderD128)` or `... with software (4 threads)`, plus `; VAAPI failed:
  <why>` when opening fell back. When a device is first opened: `kk.media: hardware decoding
  VAAPI available: /dev/dri/renderD128, driver iHD 24.1.0, libva 2.22.0 (host,
  /usr/lib/libva.so.2.2200.0)` (bundled vs host libva, and `LIBVA_DRIVER_NAME` /
  `LIBVA_DRIVERS_PATH` if set).
* Tests: `tests/media/test_hwdecode` (fault injection via `detail::injectDownloadFailures()`,
  six threads on one shared device with and without the lock). Owner test plan for the
  T14s: `docs/hw-decode-test.md`.

### Logging

`kk.media` shows info and above by default. FFmpeg errors become warnings, except known
harmless ones (mov `UDTA parsing failed`, post-seek reference complaints) and anything logged
inside an `FfmpegLogCapture`; FFmpeg warnings are debug output. Everything:
`QT_LOGGING_RULES="kk.media.debug=true"`. Qt Multimedia's FFmpeg backend (the bundle's
`QT_MEDIA_BACKEND=ffmpeg`) installs its own `av_log` callback when it initialises, so
`initFfmpeg()` re-installs ours on every call and the audio output calls
`installFfmpegLogRouting()` right after `QMediaDevices` (alpha.3 printed FFmpeg's raw lines).

### Audio accuracy

Seeks start 200 ms (or the codec's `seek_preroll`) early and discard up to the exact sample;
the buffer is anchored once per seek from the first frame's timestamp and counted from there.
For resampled sources the input is additionally aligned to a common grid point of both rates
(every 147 input samples for 44.1 kHz) so random reads are identical to contiguous reads.

### MediaCache

* Thumbnail requests are snapped to raise the hit rate: height up to a multiple of 8 px, time to
  a 0.5 s grid (≤ 135 px high) or 0.1 s grid (larger). Draw the image scaled into your rect.
* Disk layout: `<CacheLocation>/media/{thumbs,waveforms}/<sha1(abs path, mtime, size)>/…`
  (JPEG for opaque thumbnails, PNG with alpha). A changed file gets a new directory.
* Image jobs and waveform jobs run in separate pools so long waveform scans never starve
  thumbnails. Up to 4 decoders stay open between jobs so neighbouring thumbnails decode forward.
* Preview decoders decode in software up to 1920x1200 and on the hardware decoder above
  (`PreviewDecoding::BySize`, `setPreviewDecoding()`): several hardware decoders opened for a
  first filmstrip delayed the program playback's own one (first play after an import dropped
  frames), while small frames decode as fast in software; 4K HEVC is about twice as fast on
  the hardware decoder (test_mediaperf filmstripLatency).
* While the program plays (`setPlaybackActive(true)`), parked image jobs trickle (one per pool)
  and render the nearest keyframe on a software decoder instead of the exact frame, in the idle
  scheduling class (`SCHED_IDLE`; `QThread::setPriority` is a no-op for normal Linux threads).
  Those tiles are memory-only; `setPlaybackActive(false)` drops them and emits `thumbnailReady`
  so the views ask again for the exact frames (spec 76, GitHub issue 2: a page turn during
  playback of a long-GOP clip lost a quarter of the frames to exact seeks of the new tiles).
  Measure with `tests/playback/test_pageturnplayback.cpp` (`KK_PAGETURN_REPORT=1`).
* `shutdown()` (automatic on quit) drops queued jobs and waits for running ones.

`prepareFrame()` is `decodeFrame()` for the render look-ahead: it gives up between two decoded
frames when told to and leaves the decoder valid.

## Performance (dev box: i7 Comet Lake 8 threads + UHD 630 iHD 24.1 + GTX 1650, 1080p H.264, 2 s GOP)

| Path | decodeFrame (raw) | frameAt 960x540 | random seek avg |
|---|---|---|---|
| software (4 threads) | 179-204 fps | 103-126 fps | 170-240 ms (load dependent) |
| VAAPI (Intel) | 86 fps (download-bound) | 41 fps | 78 ms |
| CUDA (NVIDIA) | 343-437 fps | 104-126 fps | 48 ms (with soft seek) |
| Vulkan (NVIDIA) | 233-293 fps | 100-117 fps | 70-92 ms |

HEVC Main10 720p: SW 120-146 fps (yuv420p10), VAAPI 83 fps / CUDA 282-398 fps / Vulkan
245-326 fps (P010); HEVC seeks 17-33 ms on every hardware backend.
Run `build/test_mediaperf` to reproduce. AAC decode + resample: 800-960x real time.
Cold 180 px thumbnail of a 1080p file: ~110-150 ms.
Stills (1920x1080, `ImageLoader::load()`): JPEG 17 ms, PNG 52 ms, AVIF 89 ms, EXR float 34 ms
(was 345 ms with FFmpeg's own `apply_trc`; now a lookup table); header reads 0.1-0.2 ms;
still-cache hit 5-30 µs.

## Format matrix (docs/archive/plan.md §5.8)

Status: **tested** = a real fixture is generated by `scripts/make_test_media.sh` and probed +
decoded by `test_formats` (size, duration, non-black pixels / exact quadrant colours and
orientation, non-silent audio); **untested** = expected to work through the same library path,
no fixture (no encoder available). Results on the dev box: all tested rows pass, none skipped.

### Video (containers / codecs) — FFmpeg

| Fixture | Container / codec | Status |
|---|---|---|
| mp4_h264.mp4, m4v_h264.m4v | MP4/M4V, H.264 + AAC | tested |
| mp4_hevc.mp4 | MP4, HEVC + AAC (Main10 also in `hevc10.mp4`) | tested |
| mp4_av1.mp4 | MP4, AV1 (SVT-AV1) + AAC | tested |
| mov_prores.mov | MOV, ProRes 422 + PCM (4444 alpha in `prores4444_alpha.mov`) | tested |
| mov_dnxhr.mov, mxf_dnxhd.mxf | MOV/MXF, DNxHR SQ + PCM | tested |
| mov_cineform.mov | MOV, CineForm (cfhd) + PCM | tested |
| mkv_vp9.mkv, mkv_ffv1.mkv | MKV, VP9 + Opus / FFV1 + FLAC | tested |
| webm_vp9.webm, webm_vp8.webm, webm_av1.webm | WebM, VP9/VP8/AV1 + Opus/Vorbis | tested |
| avi_mjpeg.avi, avi_mpeg4.avi, avi_raw.avi | AVI, MJPEG + PCM / MPEG-4 + MP3 / raw yuv420p | tested |
| mts_h264.mts (and `h264_25fps.ts`) | M2TS/TS, H.264 + AC-3 | tested |
| mxf_mpeg2.mxf | MXF, MPEG-2 4:2:2 + PCM | tested |
| flv_h264.flv | FLV, H.264 + AAC | tested |
| wmv_wmv2.wmv | ASF/WMV, WMV2 + WMA2 (WMV3/VC-1 decode only: untested) | tested |
| 3gp_h263.3gp | 3GP, H.263 + AAC | tested |
| ogv_theora.ogv | Ogg, Theora + Vorbis | tested |
| mpg_mpeg2.mpg, vob_mpeg2.vob | MPEG-PS, MPEG-2 + MP2 / AC-3 | tested |
| dv_pal.dv | DV (PAL, 16:15 pixels) + PCM | tested |
| y4m_raw.y4m | YUV4MPEG | tested |
| anim.gif | animated GIF as video (FFmpeg gif demuxer) | tested |
| anim.webp | animated WebP as video (Qt plugin; FFmpeg 6.1 cannot) | tested |

### Audio — FFmpeg

| Fixture | Format | Status |
|---|---|---|
| audio.wav / .aiff / .w64 | PCM s16 / s16be / s24 | tested |
| audio.flac, audio.mp3, audio.m4a, audio.aac | FLAC, MP3, AAC in MP4 / ADTS | tested |
| audio_alac.m4a, audio.caf | ALAC in MP4 / CAF | tested |
| audio.ogg, audio.opus, audio.mka | Vorbis, Opus (Ogg / Matroska) | tested |
| audio.wma, audio.ac3, audio.eac3, audio.dts | WMA2, AC-3, E-AC-3, DTS | tested |
| .oga, .mp2, .aifc | same decoders as above | untested |

### Stills — ImageLoader (quadrant picture, colours and orientation checked)

| Fixture | Backend | Status |
|---|---|---|
| still.png .jpg .webp .bmp .gif .tiff .ico .svg | Qt | tested |
| exif_rot90.jpg | Qt, EXIF orientation 6 applied | tested (needs python3 + Pillow) |
| still.heic (irot 90 CCW) | libheif (libde265) | tested (needs python3) |
| still.avif | libheif (dav1d/aom) | tested |
| still.jxl .exr .dpx .psd .qoi .pcx .sgi .j2k .tga | FFmpeg (EXR via sRGB curve, clamped) | tested |
| raw_rggb.dng (orientation 6) | LibRaw, half and full size | tested (needs python3) |
| cr2 cr3 nef arw raf orf rw2 ... | LibRaw | untested (no free generator; same code path as DNG) |
| .jp2, .rgb, .ppm/.pgm, .svgz, .heif/.hif | as above by extension | untested |

### Image sequences

| Fixture | Pattern | Status |
|---|---|---|
| seq_png/frame_0001..0010.png (0005 missing) | `frame_%04d.png`, gap held | tested (barcode per frame) |
| seq_dot/shot.0100..0104.jpg | `shot.%04d.jpg`, start 100, 30000/1001 fps | tested |
| seq_unpadded/1..10.png | `%d.png` | tested |

## Known limitations / TODO

* TODO(media): **zero-copy GPU interop** — VAAPI surface → dma-buf → `EGLImage`, CUDA → GL
  interop, Vulkan → GL memory import. The VAAPI download (de-tiling the iGPU surface) is the
  bottleneck of the hardware path today.
* TODO(media): software seeks in long-GOP 1080p H.264 average ~200 ms (target 150 ms). Idea:
  `skip_frame = AVDISCARD_NONREF` while decoding towards a target that is still far ahead.
* HDR (PQ / HLG) and BT.2020 video is mapped to the SDR BT.709 timeline (`HdrToSdr.h`: BT.2100
  linearisation, reference white 203 cd/m2 = SDR white, BT.2020 → BT.709 primaries, a highlight
  roll-off above 0.8) in the GPU shader and in `FrameConverter`; there is no HDR output or
  per-clip input colour space choice yet. Colour tags other than the matrix, range, transfer and
  BT.2020 primaries (e.g. untagged HD clips encoded with BT.601) are taken as they are.
* Only one audio stream per file is used (the default one, else the first): a second stream
  (a camera's second microphone, a commentary track) is ignored.
* MP4 / MOV files cut off after their index are shortened to the samples present, with an
  import note; files without an index ("moov atom not found") cannot be opened.
* Display-matrix flips (mirrored phone video) are ignored; only rotation is applied.
* Audio timestamp discontinuities inside a file are not repaired (assumed contiguous).
* Animated WebP is decoded by Qt sequentially: stepping backwards re-decodes from frame 0 once
  the 128 MiB in-memory frame budget is exceeded. Animated AVIF/HEIF sequences: first image only.
* TODO(media): stills are converted to 8-bit (EXR clamped after the sRGB curve, 10-bit HEIF and
  16-bit PNG/TIFF truncated); keep float/16-bit for an HDR pipeline later.
* TODO(media): image sequences decode one file per frame on the decoder thread (1080p: JPEG
  17 ms, PNG 52 ms, EXR 34 ms per frame); add read-ahead on a worker for real-time playback.
* TODO(media): the still cache does not deduplicate concurrent misses (two threads may decode
  the same file once each).
* libheif loads its codecs as plugins (`libheif/plugins/*.so`); the AppImage must bundle them
  (or HEIC decoding fails at runtime with "No decoding plugin").
* Disk cache has no size limit/eviction yet.

## How to extend

### Add a hardware backend (e.g. Windows D3D11VA, macOS VideoToolbox)
1. Add a value to `HwAccelMode` (`HwAccel.h`) and its name in `hwAccelName()`.
2. Add a row `{mode, AV_HWDEVICE_TYPE_…, "Name"}` to `kBackends` in `HwAccel.cpp`, inside the
   right `#if` for the platform, at the position it should take in the Auto order.
3. If the backend needs device selection, extend `candidateDevices()`; if a created device may
   be unable to decode, extend `deviceCanDecode()`.
4. If the downloaded `sw_format` is new (e.g. a 4:4:4 hardware format), map it in
   `mapPixelFormat()` (`VideoFrame.cpp`) or let it take the BGRA conversion path.
5. Run `test_videodecoder` and `test_mediaperf`: both iterate over every available backend.

### Add an output pixel format to `VideoFrame`
Add the enum value and its row in the table in `VideoFrame.h`, the plane geometry in
`planeWidth/planeHeight/bytesPerPixel`, the FFmpeg mapping in `mapPixelFormat()`, and tell the
compositor's shader. Anything unmapped is converted to BGRA automatically, so this is optional.

### Add an import format
1. Add the extension to the right table in `SupportedFormats.cpp` (dialogs, filters and
   `isLikelyMedia()` follow automatically; `test_formats` checks no extension is listed twice).
2. If it is a still that its library decodes, nothing else is needed when the first backend
   for unknown image extensions (Qt) or the fall-back chain handles it. To make another
   backend go first, add the extension to `preferredBackend()` in `ImageLoader.cpp`.
3. Add a fixture to `scripts/make_test_media.sh` section 12 (via `fmt name ffmpeg-args`, which
   skips gracefully) or `make_test_images.py`, bump `FIXTURE_VERSION`, and add a row to
   `probeAndDecode_data()` in `tests/media/test_formats.cpp`. Update the matrix above.

### Add a still-image backend (library)
1. Add a value to `ImageBackend` (`ImageLoader.h`) and its name in `imageBackendName()`.
2. Create `<Name>ImageBackend.cpp` implementing `readHeader<Name>()` and `load<Name>()` from
   `ImageBackends.h` (contract: thread-safe, ARGB32 premultiplied, orientation applied, set
   `backend`); compile to stubs under `#ifdef KK_HAVE_<NAME>` when the library is optional.
3. Wire it into `readHeaderWith()` / `loadWith()` / `backendOrder()` and map its extensions in
   `preferredBackend()` (`ImageLoader.cpp`).
4. Optional library: `pkg_check_modules(... IMPORTED_TARGET ...)` + `KK_HAVE_<NAME>` define in
   `CMakeLists.txt` next to libheif/LibRaw, and a `has<Name>Support()` for tests to QSKIP.

### Add an animated-image or frame-list source
`ImageFrameList` holds the frame start times (`m_startsUs`, one past the end) and decodes frame
k on demand; add an `open…()` that fills them, extend `routeFor()` (`ImageFrameList.cpp`) to
return `MediaRoute::Animation`/`Sequence` for it, and `VideoDecoder` needs no change.

### Add a cached preview type to `MediaCache`
Follow the poster-frame pattern: an `ImageKind` value (or a new job function like
`runWaveformJob`), a memory-cache key prefix, a disk sub-directory via `diskDirectory()`,
a `…Ready(path)` signal emitted from the finish function in the cache's thread.

### Test fixtures
`scripts/make_test_media.sh` generates everything into `tests/data/generated` (tests call it
automatically). Bump `FIXTURE_VERSION` when changing it. Video fixtures carry a frame-index
barcode (see the script header; `tests/media/MediaTestUtil.h` reads it), so new seek tests can
assert the exact frame.

## Channel routing (Clip Attributes)
`AudioDecoder::setChannelMapping(ClipAttributes)`: Stereo / Mono routing of chosen source
channels through an explicit 2 x N swresample matrix (`channelMatrix()` in core); Auto keeps the
default layout. Changing it invalidates the buffered samples (the next read seeks exactly).
