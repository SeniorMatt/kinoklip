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
| `SourcePlayer::setContent()` | any private timeline for an item (the multicam viewer's angle grid) with its own media map and nested resolver; same item = keep position and transport |
| `ShuttleControl.h` | pure J/K/L state machine (rates, K+L / K+J step and held slow play) |
| `PlaybackClock.h` | pure clock: free-running or slaved to the audio device (slewed, monotonic) |
| `FrameScheduler.h` | pure request bookkeeping: look-ahead, frame dropping, scrub coalescing, cancellation |
| `AutoQuality.h` | pure Full/Half decision of the opt-in "Automatic" playback resolution |
| `AudioOutput.h` | sound device interface + `createDefaultAudioOutput()` (QAudioSink, own thread), `monotonicUs()` |
| `AudioDeviceMonitor.h` | process-wide choice of the output device (Automatic = system default, or the device chosen in Preferences, with fallback while it is missing); every QAudioSink output follows it, a playing one moves its stream without stopping; each switch is logged |
| `AudioRouting.h` | pure parts of that: `chooseAudioOutput()`, `PlayedFramesTracker` (played count across device switches), `RingPuller` (ring to device, underrun skip) |
| `AudioFeeder.h` | mixer thread (`AudioMixer`) filling the ring ahead of the device |
| `VarispeedResampler.h` | windowed-sinc resampler of the varispeed path (low-passed at 2x - 16x, so shuttling does not alias) |
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
* **With sound, at any rate**: the device is the master (shuttle, reverse and slow play are
  varispeed: each device frame carries `rate` timeline samples, resampled by `AudioFeeder`). `AudioOutput::playedFrames()` reports
  heard frames: `min(processedUSecs, frames handed over − frames still in the sink buffer)` —
  correct for backends that report "written" as well as "played" positions. Reports come in
  period-sized steps and a little late, so `PlaybackClock` runs on the wall clock and slews
  towards the reports' upper envelope (gain 0.3 when the device is ahead, 0.03 when behind,
  snap beyond 3 frames times the speed). The position never goes against the play direction.
* **Muted, no device**: free-running wall clock.
* **Audio scrubbing** (paused, `PlaybackEngine::setAudioScrubbing()`): every playhead move
  sends a snippet position to the feeder's scrub mode, which keeps the ring short (~43 ms)
  and plays ~80 ms faded snippets; the device stops 350 ms after the last move.
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

### Cuts: decoder look-ahead
The render requests look only 3-4 frames ahead, far less than a jump into another clip costs:
open + seek + decoding forward from the keyframe. Without the look-ahead the render thread did
all that when the frame was already due (`test_cutperf`, below: the picture held 300 ms and ~14
frames were lost at every cut into a long GOP, whatever the decoder).

So while playing forward at up to 2x, `FrameScheduler::prepareAhead()` sends the frames entering
the next `kCutLookAheadMs` (1 s; the worst case measured is ~0.5 s) to the compositor as
`RenderRequest::prepare`, each frame once. On the render thread `render/DecodeTargets` compares
each frame's media positions with the frame before it, so only clips really coming into view
count: on any enabled track, inside compound clips, at a transition's start, at a loop's jump
back, through a proxy or a render cache chunk. A razor cut or a jump under 100 ms in the same
file is left to the playing decoder. `DecoderPool::prepare()` then gets a decoder ready on a
decode worker thread (an idle one of that file if any, otherwise a new one) and reserves it
for that position; `acquire()` finds it when the frame comes and the frame costs no decoding.

Limits (`DecoderPool`): 3 decoders reserved, 2 preparing, 8 wishes waiting (memory: a decoder's
reference pictures and its two frames; no extra picture queue). A reservation ends when used,
when a later one is used, 250 ms after its due time, or on a seek or edit, which stop the work
within one decoded frame through the request's `cancelled` flag (`FrameScheduler::clear()`,
`setParams()` with other content). Reverse and 4x+ do not use it (spec 1.32). When a frame
comes before its preparation is done it waits for the rest. `KK_NO_CUT_LOOKAHEAD=1` turns it
off for comparisons; `KK_PLAYBACK_STATS` prints its counters in the summary.

Sound needed no change: the feeder opens each clip's audio on its own thread, 6 - 38 ms inside
the 170 ms ring (0 underrun frames at all cuts with the fake sound card).

Measured with `test_cutperf` (1080p60 H.264, `-g 250`, in-points 230 frames past a keyframe
unless noted, llvmpipe under xvfb, 3 plays per row alternating with/without on one laptop;
`KK_NO_CUT_LOOKAHEAD=1` is "before"). Lateness of the first frame after each cut against the
ordinary frames' presentation, median (max) ms; lost = frames missing of the 20 around a cut.

| Clips, FFmpeg Vulkan decoder | before late | after late | before lost | after lost |
|---|---|---|---|---|
| same file, 1.5 s (11 cuts) | 317 (327) | 4.2 (8.6) | 14 | 0 |
| two files, 1.5 s (11 cuts) | 312 (328) | 5.5 (9.1) | 14 | 0 |
| two files, 0.4 s, 100 frames from a keyframe (23 cuts) | 131 (167) | 3.9 (307) | 7 | 0 |
| two files, 0.4 s, overload (23 cuts) | 381 (1035) | 431 (1501) | 18 | 18 |

| Clips, software decoder | before late | after late | before lost | after lost |
|---|---|---|---|---|
| same file, 1.5 s | 320 (419) | 3.3 (8.7) | 14 | 0 |
| two files, 1.5 s | 227 (407) | 4.4 (9.0) | 12 | 0 |
| two files, 0.4 s, 100 frames from a keyframe | 79 (182) | 3.2 (158) | 3 | 0 |
| two files, 0.4 s, overload | 437 (990) | 141 (802) | 18 | 12 |

"Overload" needs more frames decoded per second than the decoder delivers (254 frames per 0.4 s),
which no look-ahead can fix: it stays about as bad, and its longest stall is longer (the frame
waits for a preparation competing with another; 989 ms against 377 ms with Vulkan).

### Seeking while playing
A playhead change while playing (ruler click or drag, viewer jog bar, Home/End, edit
navigation) does not stop playback (Resolve keeps playing from the new position). The engine
*parks*: `FrameScheduler::clear()` cancels the stale look-ahead (`RenderRequest::cancelled`,
skipped by the render thread if not started), the sound device stops once, the target is
rendered like a paused scrub (coalesced), and clock and sound restart from the target once it
is on screen and no further seek came for `kSeekResumeMs` (80 ms; `kSeekMaxParkMs` = 400 ms at
the latest). The state stays `Playing`. Before, every playhead change restarted the sound
device (blocking) and waited for the old look-ahead, so a drag showed no picture at all until
the mouse stopped. A frame already on screen is never rendered again when motion starts.

### Quality (playback resolution)
Resolve's *Timeline Proxy Resolution*; in Kinoklip Playback > Playback Resolution, the viewer
header combo ("Full Res"...) and Preferences > Playback, one shared value
(`src/ui/viewer/PlaybackResolution`, QSettings `playback/previewQuality`).

* **Full** (default): every frame at full resolution; when rendering cannot keep up, frames
  are dropped (never shown late), the picture stays sharp.
* **Half / Quarter**: render at most 1/2 or 1/4 of the *timeline* resolution. The render size
  is `renderSize() = min(outputSize(), timeline size × factor)`, so for an Edit-page viewer
  smaller than half the timeline, Half changes nothing.
* **Auto** (opt-in, `AutoQuality`): Full while paused; during playback Full → Half only after
  3 consecutive one-second windows in which the renderer itself was overloaded (average render
  time > 90 % of the frame interval; dropped frames alone never count: they also come from GUI
  stalls, decoding and seeks), never below Half, back to Full after 2 s with load < 50 % (a
  back-off of 5 s, doubling to 30 s, when Full keeps failing), and immediately back to Full
  for the rest of the play when Half was not at least 25 % faster (decode-bound).

Alpha.1/alpha.2 defaulted to an Auto that stepped down to Quarter of the *viewer* size after
three dropped frames (and waited up to 2 minutes to try again): a ~1000 px viewer showed
250x140 pixels, which was the owner's "pixelated while playing" (feedback alpha.2 #2). A stored
"auto" is migrated to "full" once (`prefs::previewQuality()`, revision 2).

Output size = the timeline aspect fitted into the largest registered viewer (device pixels),
never above timeline resolution; textures are requested when the GPU compositor runs and every
viewer can draw them.

### Proxies and render cache
`setSubstitutes(proxies, media)` (from `MediaManagement`) puts the proxy map into every request
and replaces the media table (offline originals play their proxy's sound); a change re-renders
like a content change. `setRenderCache(view)` only affects later requests (same pictures).
`proxiesInUse()` / `proxiesInUseChanged()` drive the viewer's "Proxy" indicator. While the
engine plays, `MediaManagement` pauses background caching.

### Meters (Audio page)
The feeder thread hands its mixer a `MixLevels` and runs a `dsp::LoudnessMeter` over every mixed
chunk; each chunk becomes a time-stamped `MeterFrame` in a small mutex-guarded ring (32 chunks,
copied in microseconds). While the audio device is the clock, every engine tick picks the frame
of the sample being heard (`AudioFeeder::meterFrameAt()`, the mix runs ~170 ms ahead) at most
every `kMeterIntervalMs` (33 ms) and emits `metersUpdated()`; `meters()` returns it. Stopping
emits one silent frame (the integrated loudness and the true-peak maximum stay until
`resetLoudness()`). Without a sound device (`KK_NO_AUDIO`, tests) there are no readings.
Edits while playing (spec 48.7): the clock runs on. A changed picture cancels the frames in
flight and renders the frame due again; `followRange()` moves the end of the play range (a loop
whose range changed restarts its clock). `noteEditCommitted()` (the program engine only, from
`Project::editCommitted`) compares against the last committed timeline with
`render::sameArrangement()`: an edit stops playback with `stopOnEdit` (spec 48.8), otherwise, if
it changed the sound the ring holds, `AudioFeeder::remixUnheard()` mixes it again. That is a
handshake, because only the device thread knows how far it has read: the feeder asks and stops
writing, the next `AudioRing::read()` keeps 2048 frames (faded out) and moves the write counter
back, and the feeder steps its position back by as many frames. Ring frame n stays frame n of
the play, so nothing about the clock changes and the device is not restarted.

Sound-only edits (`render::samePictures()`) only go to the feeder: the renderer keeps its
snapshot, so moving a fader while playing never drops pictures.

### Statistics
`Stats` (fps, drops, render time and its stages: decode, upload, composite, output; seeks).
`KK_PLAYBACK_STATS=1` (any value but empty/0/false/off/no) logs one line per second while
playing and a summary every time playback stops, e.g.

```
kk.playback: KK_PLAYBACK_STATS: one stats line per second while playing and a summary when playback stops
kk.playback: stats: 60.0 fps (of 60.00), 0 dropped, quality 1 999x562, render 11.1 ms
             [decode 0.1, upload 0.9, composite 0.2, output 9.0], audio clock, A/V 0.00
kk.playback: stats summary: played 4.20 s, 252 frames shown (60.0 fps of 60.00), 0 dropped,
             0 seeks while playing, render avg [decode 0.1, upload 0.9, composite 0.2, output 9.0] ms
```

The per-second line needs one full second of uninterrupted playback (no seek-while-playing
park in between); alpha.3 had only that line, so short plays printed nothing at all. The first
line confirms the variable reached the process. `tests/playback/test_playbackstats` checks
both lines; `KK_SCREENSHOT_PLAY_MS=2000 kinoklip --screenshot edit out.png project.kkproj`
plays headless (also in the bundle).

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
`postrollMs`, `returnAfterPlayAround`, `stopOnPlayheadMove`, `stopOnEdit`) so interface profiles can swap
them. A playhead move while playing (`seekWhilePlaying()`) parks and resumes from the new frame
(Resolve); with `stopOnPlayheadMove` it pauses there instead (Premiere, spec 48.2). `stopOnEdit`
pauses after an edit during playback (spec 48.8).

## Measurements

`tests/playback/test_playbackperf` (GL under xvfb when available):
* `singleClipPlayback`: one clip on a timeline of its own size/rate, Edit-page viewer
  (1000x562), Full; stage times. More files with `KK_PERF_MEDIA=a.mp4:b.mov`
  (`scripts/make_perf_media.sh` writes screen recordings 1080p60/1440p60 and a 4K HEVC clip),
  other modes with `KK_PERF_QUALITY=auto|half|quarter`.
* `sustainedPlayback`: 1080p25 timeline (1080p H.264 under a 720p PiP with drop shadow, a
  title, linked audio; fake sound card as master).
* `seekWhilePlaying`: click (6 jumps) and drag (40 moves, 16 ms apart) latency until a frame
  within half a second of the target is on screen.

Numbers and before/after tables: `src/ui/viewer/readme.md` (Performance).

## Tests

* `test_playbacklogic` — ShuttleControl sequences, clock monotonicity and A/V bound with a
  simulated device (stale, period-quantised reports, ±0.2 % crystal drift), ring, scheduler
  bounds / dropping / reverse / loop keys / scrub coalescing / parameter changes /
  cancellation, AutoQuality (sustained overload only, never below Half, quick recovery,
  back-off, gives up when Half does not help).
* `test_playbackengine` — engine against the real Compositor and a fake sound card: states,
  J/K/L, K+L step, slow play, loop, play in-to-out, end handling, playhead write-back, scrub
  coalescing (60 moves → 2 renders), seek while playing (click: target on screen, keeps
  playing, no state change; drag: 60 moves → ≤ 2 renders), re-render on edits, output and
  proxy render size, audio master clock
  (|position − heard| < 1.5 frames), clock switching, dead-device fallback.
* `test_viewerpanel` — viewers (src/ui/viewer), source marks, F9 integration.
* `test_cutlookahead` — decoder look-ahead at cuts (spec 1.29 - 1.34): decoded-frame counts and
  counters where possible, delivery on time with a stated margin, cancelling, limits, J/K/L.
* `test_cutperf` — the measurement above (prints, asserts nothing; `KK_CUTPERF_DIR` keeps the
  generated media, `KK_CUTPERF_RUNS`, `KK_CUTPERF_SIZE`).
* `test_playbackperf` — playback and seek measurements (see above).

## How to extend

* **A new transport command**: add the action (StandardActions.cpp + scripts/keymap-resolve-windows.py, which writes resources/keymaps/resolve-windows.json),
  a slot on `PlaybackEngine`, and one `on("id", ...)` line in `ViewerTransport::connectActions()`.
* **A new convention switch**: add a field to `PlaybackBehaviour` (+ its QSettings key in
  `fromSettings()`), read it where the behaviour happens; never an inline constant.
* **Another sound backend**: implement `AudioOutput` (the contract is in its header: heard
  position + underrun skipping) and pass it in `PlaybackEngine::Options::audio`.
* **Pitch-kept fast play**: replace `VarispeedResampler` in `AudioFeeder::mixVarispeed()` by a
  time-stretcher (the window and the clock already handle every rate).

## Known gaps / TODO

* TODO(playback): the look-ahead's 1 s does not grow with the GOP: a preparation taking longer
  (10 s GOPs, 4K) still saves what it finished, the frame waits for the rest. Media a
  composition's nodes add and time-effect frames are not prepared.
* TODO(playback): seeks show the exact target frame (decoder seek + decode forward from the
  keyframe, 70-140 ms here); showing the nearest keyframe first would be faster for long GOPs.
* TODO(playback): presentation is timer-driven (4 ms) rather than locked to the display's
  vsync (`QOpenGLWidget::frameSwapped`); frame pacing is good to ±4 ms.
* TODO(playback): a device switch (headphones plugged in) is picked up on the next play only.
