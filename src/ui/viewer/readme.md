# src/ui/viewer — source and timeline viewers

Owned by the playback agent. Widgets on top of `src/playback` (engine, source player).

| File | What |
|---|---|
| `ViewerPanel.h` | the viewer panel: `Mode::Source` / `Mode::Timeline`; header "Source — clip" / "Timeline — name" with zoom (Fit/50/100/200 %), playback resolution ("Full Res" / "Half Res" / "Quarter Res" / "Auto Res", see `PlaybackResolution`) and timecode; picture; scrub bar (scrubbing while playing keeps playing); transport bar |
| `PlaybackResolution.h` | the application-wide playback resolution (Resolve's Timeline Proxy Resolution): Playback > Playback Resolution actions, every viewer's combo and Preferences > Playback stay in sync; stored in `playback/previewQuality` (default Full, old "auto" migrated once) |
| `ViewerSurface.h` | picture-area interface; `ViewerSurface::create()` picks GL or raster; `ViewerPicture` = shared state, letterbox/zoom maths, raster drawing |
| `GlViewerSurface.h` | `QOpenGLWidget`: draws the compositor's texture **zero copy** (`glWaitSync` on the frame's fence, sampler object, frame kept alive until the next one is drawn) or uploads an image (`glTexSubImage2D` from ARGB32 memory) |
| `RasterViewerSurface.h` | `QWidget` fallback (offscreen screenshots, CI, no GPU, `KK_NO_OPENGL=1`); draws `RenderedFrame::image` |
| `ScrubBar.h` | jog/scrub strip: duration, in/out band with brackets, playhead; drag to scrub, wheel = one frame |
| `ViewerTransport.h` | routes the playback-owned global actions to the **active** viewer, J/K/L hold detection, full-screen viewer |

## Behaviour

* **Timeline viewer** shows `AppContext::playback` (the program engine created in `main()`).
  It registers as an output client with its picture size in device pixels, so the engine
  renders at viewer size (zoomed: up to timeline resolution) and requests textures when the
  surface is GL. Without an engine (tests) it only follows the playhead.
* **Source viewer** creates a `SourcePlayer` on the first `setSourceMedia()` (sharing the
  program engine's Compositor). Marks are the clip's own (remembered per clip). With focus
  inside it, the `mark.*` keys and Home/End act on the clip: the panel accepts the
  `QEvent::ShortcutOverride` of keys bound (in the ActionRegistry) to
  `ViewerPanel::sourceKeyActionIds()`, so the timeline's global actions do not fire.
  Dragging the picture starts a `mime::SourceRange` drag (marked range, or the whole clip).
  A removed media item unloads; "Audio only" is shown for clips without video.
* **Active viewer**: the last viewer that got focus or a click (`activate()`); focus entering
  a widget with the dynamic property `kkProgramFocus` (EditPage tags the timeline panel)
  activates the visible Timeline viewer. Global transport actions go to the active viewer if
  visible, else the first visible Timeline viewer, else any visible viewer, else the program
  engine. So on the Media page Space plays the preview; on the Edit page it plays whichever
  viewer was used last, like Resolve.
* **Transport buttons** act on their own viewer directly (and activate it). Loop is the one
  global `transport.loop` toggle, applied by every viewer to its engine. Tooltips show the
  current shortcut from the registry.
* **Full-screen viewer** (`view.fullscreen_viewer`, Ctrl+F): a picture-only Timeline viewer
  window on the main window's screen; all registry actions are added to it; Esc closes.
* **Single viewer mode** (`view.single_viewer`, app-owned) hides the source viewer;
  opening a clip from the media pool shows it again.

## Actions connected here (ViewerTransport)

`transport.play_pause` (Space), `transport.shuttle_reverse/stop/forward` (J/K/L, auto-repeat
off), `transport.prev_frame/next_frame` (arrows), `transport.back_1s/forward_1s`
(Shift+arrows), `transport.play_in_out` (Alt+/), `transport.play_around` (/), `transport.play_reverse` (Shift+Space),
`view.fullscreen_viewer` (Ctrl+F). **Not** `transport.prev_edit/next_edit/go_to_start/go_to_end`
(timeline module) and not `transport.loop` (each viewer applies the toggle).

## Performance

`tests/playback/test_playbackperf` — 1080p25 timeline (1080p H.264 + 720p PiP with drop
shadow + title, linked audio), fake sound card as master clock, 6 s:

| Backend | Output | Quality | fps | dropped / 150 | render ms/frame |
|---|---|---|---|---|---|
| GTX 1650 (offscreen EGL) | 1000x562 (Edit viewer) | Auto (stays Full) | 25.00 | 0 | 2.0 |
| GTX 1650 (offscreen EGL) | 1000x562 | Full | 25.00 | 0 | 2.1 |
| GTX 1650 (offscreen EGL) | 1920x1080 (full screen) | Auto (stays Full) | 25.00 | 0 | 2.0 |
| llvmpipe (xvfb), quiet machine | 1000x562 | Auto / Full | 25.00 | 0 | 8-10 |
| llvmpipe (xvfb), quiet machine | 1920x1080 | Auto (Full/Half) | 25.00 | 0 | 27 |
| llvmpipe (xvfb), load average 10-16 | 1000x562 | Full | 24.83 | 0 | 46 |
| llvmpipe (xvfb), load average 10-16 | 1920x1080 | Auto (-> Quarter) | 20.99 | 33 | 18 (Quarter) |

The machine was shared with several parallel builds; software GL suffers most from that.
Audio was the master clock in every run (fake device, 10 ms periods, 40 ms latency); A/V
error reported by the clock < 0.001 frames, no underruns.

## Tests

`tests/playback/test_viewerpanel.cpp` (offscreen, raster path): program frames reach the
picture, header/timecode/scrub/marks, zoom and quality boxes, source clip loading and its
I/O/Home/Shift+I keys, marks remembered per clip, transport routing to the active viewer,
loop toggle, `kkProgramFocus`, full-screen viewer, F9 inserting the marked source range.
`tests/app/test_glviewer.cpp` checks real GL pixels (run under xvfb).

## How to extend

* **A transport button**: `addButton(bar, "<action id>", icon, tr("Tip"), handler)` in the
  ViewerPanel constructor; the handler should call `activate()` and act on `engine()`.
* **An overlay** (safe areas, timecode burn-in): draw it in `ViewerPicture::paintTexts()` for
  the raster path and as a transparent child widget / extra GL pass in `GlViewerSurface`.
* **Panning when zoomed**: keep an offset in `ViewerPicture` and add it in `pictureRect()`;
  middle-drag in `ViewerPanel::eventFilter()`.

## Known gaps

* TODO(viewer): no panning when zoomed in (the picture stays centred).
* TODO(viewer): dragging a source range shows a grab of the widget (the raster path draws
  the frame; on GL it is the widget's last frame).
* TODO(viewer): no safe-area / overlay guides, no A/B comparison.
