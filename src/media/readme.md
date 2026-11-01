# `src/media` — media engine

Everything that touches media files: probing, frame-exact video decoding (software and GPU),
sample-exact audio decoding, and cached UI previews (thumbnails, poster frames, waveforms).
Namespace `kk::media`, logging category `kk.media`. Depends only on `core` (for `Rational`),
QtCore/QtGui and FFmpeg — never on Qt Widgets.

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
| `MediaInfo.h` | `MediaInfo` struct + `probe(path)` (synchronous, any thread). Stills report `isStillImage`, `durationUs = 0`. |
| `VideoDecoder.h` | `decodeFrame(timeUs)` → `shared_ptr<const VideoFrame>` (raw planes, GPU compositor path); `frameAt(timeUs, maxSize)` → `QImage` ARGB32_Premultiplied (thumbnails, tests). `Accuracy::{Exact, NearestKeyframe}`. Not thread-safe; one per thread. |
| `VideoFrame.h` | Raw frame: `PixelFormat` (NV12, P010, YUV420P/422P/444P, 10-bit planar, BGRA premultiplied), plane pointers/strides, colour matrix/range/transfer, pts, duration, SAR, rotation. Immutable, refcounted, may outlive the decoder. |
| `HwAccel.h` | `HwAccelMode {Auto, Off, Cuda, Vaapi, Vulkan}`, `setPreferredHwAccel()`, `hwCapabilities()` (for Preferences). |
| `AudioDecoder.h` | `read(startSample48k, dst, frames)`: sample-accurate, zero-filled outside the media. Not thread-safe. |
| `MediaCache.h` | Singleton: `thumbnail()`, `posterFrame()`, `waveform()` — return cached result or null + async job + signal. Memory LRU + disk cache. |
| `WaveformData.h` | min/max peaks per 1/100 s and channel, scaled to −127..127; compute/save/load. |
| internal | `FFmpegUtil` (RAII, open/seek helpers, log routing), `HwDevices`/`HwAccel.cpp` (backend table, shared device contexts), `VideoFrameFactory` (hw download + pooling), `FrameConverter` (swscale → QImage). |

## Data flow

```
                    ┌──────────── VideoDecoder ────────────────────────────────────┐
file ─ libavformat ─┤ seek / decode-forward state machine (cur + look-ahead frame) │
                    │   libavcodec (SW ≤4 threads, or VAAPI/CUDA/Vulkan hwaccel)   │
                    │      │ only the frame actually returned is materialised:    │
                    │      ├─ decodeFrame(): hw surface → av_hwframe_transfer_data │
                    │      │    → pooled NV12/P010 │ SW frame shared by reference  │
                    │      └─ frameAt(): swscale (colour matrix/range, scaling)    │
                    │                    → QImage ARGB32_Premultiplied + rotation  │
                    └──────────────────────────────────────────────────────────────┘
file ─ libavformat ─ AudioDecoder ─ libavcodec ─ swresample (48k stereo float) ─ read()
MediaCache: QThreadPool jobs → (disk cache | VideoDecoder/AudioDecoder) → memory LRU → signal
```

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
* H.264 on CUDA/Vulkan: FFmpeg's flush re-creates the whole hw decoder (~250 ms). When the
  packet at the new position is an IDR we skip the flush ("soft seek") and drop stale frames
  via packet→frame `opaque` generation tags. VAAPI re-inits in ~5 ms and always flushes.
* Global preference: `setPreferredHwAccel()`; per decoder: `VideoDecoder::setHwAccel()`;
  for debugging without touching settings: environment `KINOKLIP_HWACCEL=off|auto|vaapi|cuda|vulkan`.
* Linux permissions: VAAPI/Vulkan need access to `/dev/dri/renderD*` (member of group
  `render`, or the active seat user). Without it they are reported unavailable.

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
* `shutdown()` (automatic on quit) drops queued jobs and waits for running ones.

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

## Known limitations / TODO

* TODO(media): **zero-copy GPU interop** — VAAPI surface → dma-buf → `EGLImage`, CUDA → GL
  interop, Vulkan → GL memory import. The VAAPI download (de-tiling the iGPU surface) is the
  bottleneck of the hardware path today.
* TODO(media): software seeks in long-GOP 1080p H.264 average ~200 ms (target 150 ms). Idea:
  `skip_frame = AVDISCARD_NONREF` while decoding towards a target that is still far ahead.
* TODO(media): HDR (PQ/HLG) is reported in `VideoFrame::transfer` but not tone-mapped.
* Display-matrix flips (mirrored phone video) are ignored; only rotation is applied.
* Audio timestamp discontinuities inside a file are not repaired (assumed contiguous).
* Animated GIF/WebP: only what FFmpeg decodes (FFmpeg 6.1 cannot decode animated WebP).
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

### Image sequences (`frame_%04d.png`)
Open them through FFmpeg's `image2` demuxer: pass the pattern path and a frame rate
(`AVDictionary` option `framerate` in `openInput()`), and make `probe()` treat the pattern as a
clip instead of a still (`isImageDemuxer()` currently marks single images as stills). The
decoder needs no changes — the demuxer delivers timestamps like any video stream.

### Add a cached preview type to `MediaCache`
Follow the poster-frame pattern: an `ImageKind` value (or a new job function like
`runWaveformJob`), a memory-cache key prefix, a disk sub-directory via `diskDirectory()`,
a `…Ready(path)` signal emitted from the finish function in the cache's thread.

### Test fixtures
`scripts/make_test_media.sh` generates everything into `tests/data/generated` (tests call it
automatically). Bump `FIXTURE_VERSION` when changing it. Video fixtures carry a frame-index
barcode (see the script header; `tests/media/MediaTestUtil.h` reads it), so new seek tests can
assert the exact frame.
