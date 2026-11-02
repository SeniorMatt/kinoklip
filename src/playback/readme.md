# src/playback — playback engine and source player

Plays a `kk::Timeline` with sound and picture in sync: transport state machine, J/K/L
shuttle, loop, audio-master clock, frame look-ahead through the `Compositor`, preview quality.
Depends on `core`, `media` (indirectly) and `render`; never on widgets. Namespace `kk`, logging
category `kk.playback`.

## Public API

| Header | What |
|---|---|
| `PlaybackEngine.h` | `PlaybackEngine` (QObject): content, transport slots, output clients, quality, stats; `PlaybackBehaviour` (convention data) |
| `SourcePlayer.h` | `SourcePlayer`: one media item on a private one-clip timeline, own in/out marks, `selection()` -> `SourceRange`; `makeSourceTimeline()` |
| `ShuttleControl.h` | pure J/K/L state machine (rates, K+L / K+J step and held slow play) |
| `PlaybackClock.h` | pure clock: free-running or slaved to the audio device (slewed, monotonic) |
| `FrameScheduler.h` | pure request bookkeeping: look-ahead, frame dropping, scrub coalescing |
| `AudioOutput.h` | sound device interface + `createDefaultAudioOutput()` (QAudioSink, own thread), `monotonicUs()` |
| `AudioFeeder.h` | mixer thread (`AudioMixer`) filling the ring ahead of the device |
| `AudioRing.h` | lock-free SPSC ring of stereo float frames |

```cpp
kk::PlaybackEngine program;                         // owns a Compositor (+ render thread)
program.setMediaPool(project.mediaPool());
program.setTimeline(project.timeline());            // follows edits and the playhead
connect(&program, &kk::PlaybackEngine::frameReady, surface, &ViewerSurface::setRenderedFrame);
program.setOutputClient(viewer, pictureSizeInDevicePixels, surface->canDrawTextures());
program.play(); program.shuttleForward(); program.step(-1); program.setLoop(true);

kk::SourcePlayer source(program.compositor());      // shares the render thread / GL context
source.setMedia(pool->item(id));
source.markInAtPosition(); ... source.selection();  // {mediaId, inUs, outUs}
```

`main()` creates the program engine and puts it into `AppContext::playback`; the viewers
(`src/ui/viewer`) create source players on demand.

## Data flow

```
             GUI thread                                   render thread (Compositor)
 Timeline ──changed/playhead──► PlaybackEngine ──renderAsync (2-3 ahead)──► GL / CPU backend
   ▲                             │   ▲ tick 4 ms      ◄──future (texture+fence | image)──┘
   └──setPlayhead (≤30 Hz)───────┘   │ FrameScheduler.poll(due) -> frameReady -> viewer
                                     │
                       PlaybackClock ◄── playedFrames() ── AudioOutput (QAudioSink, kk.audio thread)
                                                                ▲ pull (readData)
                       AudioFeeder thread ──AudioMixer.mix──► AudioRing (lock-free, 170 ms)
```

### Clock
* **Forward 1x with sound**: the device is the master. `AudioOutput::playedFrames()` reports
  heard frames: `min(processedUSecs, frames handed over − frames still in the sink buffer)` —
  correct for backends that report "written" as well as "played" positions. Reports come in
  period-sized steps and a little late, so `PlaybackClock` runs on the wall clock and slews
  towards the reports' upper envelope (gain 0.3 when the device is ahead, 0.03 when behind,
  snap beyond 3 frames). The position never goes backwards.
* **Reverse, shuttle (2x-8x), slow (0.5x), muted, no device**: free-running wall clock.
* A device that reports no progress within 600 ms is abandoned for the wall clock (logged).
* Underruns: the device plays silence and later skips the same number of ring frames, so
  device frame n always carries timeline sample `start + n` (sync survives hiccups).
* Loops: the feeder wraps the mix at the loop end sample; the clock stays unwrapped and the
  engine wraps it (`FrameSlot::key` = unwrapped, `frame` = wrapped), so loops are seamless.

### Video
Every 4 ms the engine reads the clock, asks `FrameScheduler` to keep up to 3 requests ahead
(the frames due at the next display intervals; further ahead when rendering is slower than
real time) and presents the newest finished frame that is due. Older finished frames are
dropped, never shown late. A frame already on screen is never requested again.
While paused, playhead moves and edits go through `requestStill()`: one request in flight,
later wishes only replace the pending frame number (a fast scrub renders what it can and
always ends on the last position).

### Quality
`QualityMode::Auto` renders Full while paused; during playback it evaluates every 500 ms
(after a 1 s settle) and steps down (Full → Half → Quarter) when the average render time
exceeds 85 % of the frame interval or ≥ 3 frames were dropped in the window, and back up after
2 s below 35 %. Manual Full/Half/Quarter are fixed. Output size = the timeline aspect fitted
into the largest registered viewer (device pixels), never above timeline resolution; textures
are requested when the GPU compositor runs and every viewer can draw them.

## Transport semantics (Resolve-like)

| Input | Result |
|---|---|
| Space | play / pause (on the last frame: start over, `PlaybackBehaviour::restartAtEnd`) |
| L / J | 1x, 2x, 4x, 8x forward / reverse; the other direction restarts at 1x |
| K | pause |
| K held + tap L / J | one frame forward / back |
| K held + hold L / J | after 300 ms slow play ±0.5x until a key is released |
| ← / → , Shift+← / → | one frame / one second (pauses) |
| loop (Ctrl+/) | repeat [in, out) if both marks are set, else the whole timeline |
| play in to out (Alt+/) | the marked range once (looping if loop is on) |
| play around (/) | pre-roll .. post-roll around the playhead (2 s each), then back |

Keys come from the ActionRegistry keymap (nothing is hard-coded); conventions that differ
between editors live in `PlaybackBehaviour` (QSettings `playback/restartAtEnd`, `prerollMs`,
`postrollMs`, `returnAfterPlayAround`) so interface profiles can swap them.

## Measurements

`tests/playback/test_playbackperf` (1080p25 timeline: 1080p H.264 under a 720p PiP with drop
shadow, a title, linked audio; fake sound card as master): 25.00 fps with 0 dropped frames on
the GTX 1650 at viewer and full-HD size (2 ms/frame) and on llvmpipe at viewer size; full-HD
on llvmpipe needs Auto quality (Half). Full table: `src/ui/viewer/readme.md`.

## Tests

* `test_playbacklogic` — ShuttleControl sequences, clock monotonicity and A/V bound with a
  simulated device (stale, period-quantised reports, ±0.2 % crystal drift), ring, scheduler
  bounds / dropping / reverse / loop keys / scrub coalescing / parameter changes.
* `test_playbackengine` — engine against the real Compositor and a fake sound card: states,
  J/K/L, K+L step, slow play, loop, play in-to-out, end handling, playhead write-back, scrub
  coalescing (60 moves → 2 renders), re-render on edits, output size, audio master clock
  (|position − heard| < 1.5 frames), clock switching, dead-device fallback.
* `test_viewerpanel` — viewers (src/ui/viewer), source marks, F9 integration.
* `test_playbackperf` — sustained playback measurement (GL under xvfb when available).

## How to extend

* **A new transport command**: add the action (StandardActions.cpp + keymap_default.json),
  a slot on `PlaybackEngine`, and one `on("id", ...)` line in `ViewerTransport::connectActions()`.
* **A new convention switch**: add a field to `PlaybackBehaviour` (+ its QSettings key in
  `fromSettings()`), read it where the behaviour happens; never an inline constant.
* **Another sound backend**: implement `AudioOutput` (the contract is in its header: heard
  position + underrun skipping) and pass it in `PlaybackEngine::Options::audio`.
* **Audio while shuttling** (scrub audio, 2x with pitch): extend `AudioFeeder` with a rate
  and make `startMotion()` use the audio clock for those rates.

## Known gaps / TODO

* TODO(playback): no audio in reverse / shuttle / slow play (wall clock, silent).
* TODO(playback): no audio scrubbing (short snippets while dragging).
* TODO(playback): presentation is timer-driven (4 ms) rather than locked to the display's
  vsync (`QOpenGLWidget::frameSwapped`); frame pacing is good to ±4 ms.
* TODO(playback): a device switch (headphones plugged in) is picked up on the next play only.
