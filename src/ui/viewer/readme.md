# src/ui/viewer — source and timeline viewers

Owned by the playback agent. Widgets on top of `src/playback` (engine, source player).

| File | What |
|---|---|
| `ViewerPanel.h` | the viewer panel: `Mode::Source` / `Mode::Timeline`; header "Source — clip" / "Timeline — name" with zoom (Fit/50/100/200 %), playback resolution ("Full Res" / "Half Res" / "Quarter Res" / "Auto Res", see `PlaybackResolution`) and timecode; picture; scrub bar (scrubbing while playing keeps playing); transport bar |
| (ViewerPanel) | `setSourceContent(item, timeline)` shows a private timeline as a source "clip" (the Cut page's source tape; marks / position remembered per pseudo item id, no drag out); `setMulticamAllowed(false)` hides the Source / Multicam mode menu (Cut page viewers) |
| `PlaybackResolution.h` | the application-wide playback resolution (Resolve's Timeline Proxy Resolution): Playback > Playback Resolution actions, every viewer's combo and Preferences > Playback stay in sync; stored in `playback/previewQuality` (default Full, old "auto" migrated once) |
| `ViewerSurface.h` | picture-area interface; `ViewerSurface::create()` picks GL or raster; `ViewerPicture` = shared state, letterbox / zoom / pan maths, raster drawing (zoomed: only the visible part of the frame is scaled) |
| `GlViewerSurface.h` | `QOpenGLWidget`: draws the compositor's texture **zero copy** (`glWaitSync` on the frame's fence, sampler object, frame kept alive until the next one is drawn) or uploads an image (`glTexSubImage2D` from ARGB32 memory) |
| `RasterViewerSurface.h` | `QWidget` fallback (offscreen screenshots, CI, no GPU, `KK_NO_OPENGL=1`); draws `RenderedFrame::image` |
| `MaskOverlay.h` | on-screen mask controls of the timeline viewer: an additive transparent child of the picture surface (outlines, handles, drawing / editing gestures of the current clip's masks; follows `pictureRect()` and the clip transform); `createModeButton()` is the bottom-left on-screen controls menu (Off / Mask, `view.onscreen_masks`). See docs/behavior-spec.md section 27 |
| `ColorPickOverlay.h` | Timeline mode: the colour eyedropper of effect parameters (Chroma Key's Key Color, spec 40.3): active while `SelectionModel::colorPicker()` names an effect of a clip under the playhead; click = 3 x 3 pixels, drag = the rectangle's mean; samples the frame rendered with that effect and the later ones switched off (program compositor, `setFrameSource()` in tests); one undo step, then off; Esc cancels |
| `EditOverlay.h` | Timeline mode: the edit types drawn over the picture while media is dragged onto the viewer (spec 4.14); `ViewerEditAids.cpp` turns the drop into `sourceEditDropped(actionId, sources)`; `setEditTileActions()` replaces the tiles (the Cut page's smart edits, spec 35.26). `ViewerEditAids.cpp` also builds the viewer's context menu (`buildContextMenu()`, spec 24.7) |
| `TransformOverlay.h` | Timeline mode: on-screen Transform and Crop controls (`view.onscreen_transform`, `view.onscreen_crop`, entries of the on-screen controls menu): outline, corner / edge scale handles, rotation knob, anchor cross, move; crop edges and corners; one named undo step per drag (spec 24.9, 24.11, 24.12); edges, centre, anchor and crop edges snap to guides within 6 px (`setGuideOverlay()`, Ctrl skips; spec 41.9) |
| `GuideOverlay.h` | both modes: guides, safe areas (action 90 %, title 80 %, centre mark), aspect marker and grid drawn over the picture (frame px through `pictureRect()`), never rendered; mouse-transparent, it filters the events of the surface and its child overlays so a guide within 4 px wins (hover cursor, drag, double-click dialog, context menu); timeline guides are `TimelineData::guides` (undo steps, project format 14), source guides per user; `setSuppressed()` hides every overlay for picture grabs and `--screenshot` (spec 41) |
| `ViewerRuler.h` | the pixel rulers along the picture area's top and left edges (View > Show Rulers): frame px, 1 / 2 / 5 steps, guide marks; dragging out of a ruler adds a guide (`GuideOverlay::beginNewGuide()`) |
| `ViewerOverlaySettings.h` | the user's overlay options (show / lock / snap guides, colour, rulers, safe areas, aspect marker, grid; QSettings `viewer/overlays/*`) and the View menu bindings: View > Show Rulers, Guides, Safe Area, Show Grid, Grid Type; guide commands act on `targetOverlay()`; preset menus (`attachPresetMenus()`, presets in `<AppData>/guide-presets.json`) |
| `TransformGesture.h` | pure maths of those controls: picture geometry (fit size, crop rectangle), handle positions and hit-testing, `TransformDrag` (what a drag writes into `ClipData::transform`, the Shift conventions) |
| `ViewerNavigation.cpp` | ViewerPanel part: zoom and pan (spec 41.18 - 41.24): mouse wheel zooms about the cursor (12.5 % a notch, 10 % - 1600 %), middle drag or Space + left drag pans, Shift+Z with focus in the viewer / header Fit / middle double-click fit; the header shows a custom percentage; an application event filter (`ViewerNavigationFilter`) sees the input before the overlays over the picture. Zoom maths (`panForZoomAbout()`, `clampPan()`) are in `ViewerPicture` |
| `ViewerOffline.cpp` | ViewerPanel part: the "Media Offline" notice when the clip shown has no file (`MediaPool::isOffline()`, spec 24.8) |
| `TrimTwoUpView.h` | Timeline mode: the two-up trim view (spec 8.15, 8.19): exact source frames from `MediaCache::frame()` (default), or with Preferences > Editing "Trim view shows effects and transforms" each clip alone rendered by the program compositor (`setProgramSource()`, `Pictures::Program`, GPU only); one request in flight per side; `ViewerPanel::setTrimPreview()` |
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
* **Single viewer mode** (`view.single_viewer`, app-owned; EditPage adds a mode button at
  the right end of both viewer headers): one viewer shows the timeline, or the source clip
  after one was opened (media pool, match frame); Q swaps; activating the timeline viewer
  shows the timeline (spec 24.4 - 24.6).

* **On-screen mask controls** (Timeline viewers only): `MaskOverlay` sits on the picture
  surface and is shown while a mask of the selected clip is active (Inspector Masks section)
  or the Mask on-screen controls are on, with the playhead inside that clip. It takes the mouse
  only then; otherwise clicks reach the surface as before. Zoom and aspect changes call
  `MaskOverlay::refresh()`.

## Multicam viewer (ViewerMulticam.cpp, MulticamOverlay)

Source viewers have a Source / Multicam mode (header combo, View > Multicam Viewer) and a
grid size combo (Auto, 2x2, 3x3, 4x4). In Multicam mode the source player plays
`multicam::gridTimeline()` (SourcePlayer::setContent()) of the multicam clip under the
timeline playhead — following it on every playhead change, also during playback (paused
engine, coalesced still renders) — else of the loaded multicam clip (opening one from the
media pool shows its grid until the timeline playhead is moved). `MulticamOverlay` draws the
angle labels, the active angle frames (red both, blue video, green audio) and the page arrows;
a click or the `multicam.angle_n` keys (1-9, only while the mode is on) emit
`multicamAngleChosen()`, which EditPage connects to `TimelineController::chooseMulticamAngle()`
(cut or switch per Clip > Multicam). Not following a timeline clip, a choice changes the
angle the viewer plays.

Grid cost: one nested composite per angle at cell size; decoding is per angle at the source
size, or the proxy's when proxies are linked (Playback > Proxy Handling, the source player
gets the program's proxy map). Measured (KK_PLAYBACK_STATS, 4 x 1080p25 H.264 angles, CUDA
decode, 552x311 viewer, llvmpipe software GL under Xvfb): 10-11 fps, render 85 ms = decode
16 + upload 10 + composite 53 + output 7 ms. For comparison one 1080p clip composites in
0.1-1.5 ms on the same software GL, so the cost is the four nested passes (an RGBA16F target
per angle, see src/render/readme.md); not measured on a hardware GPU (none usable headless).
TODO(viewer): composite the grid cells straight into the output (no nested target per angle)
and decode angles at reduced size when no proxy exists.

## Actions connected here (ViewerTransport)

`transport.play_pause` (Space), `transport.shuttle_reverse/stop/forward` (J/K/L, auto-repeat
off), `transport.prev_frame/next_frame` (arrows), `transport.back_1s/forward_1s`
(Shift+arrows), `transport.play_in_out` (Alt+/), `transport.play_around` (/), `transport.play_reverse` (Shift+Space),
`view.fullscreen_viewer` (Ctrl+F). **Not** `transport.prev_edit/next_edit/go_to_start/go_to_end`
(timeline module) and not `transport.loop` (each viewer applies the toggle).

## Performance

`tests/playback/test_playbackperf` (see src/playback/readme.md). Playback2 work (alpha.2
feedback "pixelated while playing"), 2026-11-02, llvmpipe under xvfb, Edit-page viewer
1000x562, 6 s per case. The machine was shared with parallel builds (load average 8-15), so
software GL numbers vary by ±10 fps between runs; the GTX 1650 / Intel GPUs were not usable for
GL by the agents (no display session), decoding used CUDA ("hw") or software ("sw").

`singleClipPlayback` (one clip, timeline of its own size/rate):

| File | Decode | Before: default Auto | After: default Full |
|---|---|---|---|
| perf_1080p (H.264 1080p25) | hw | 25.0 fps, 0 dropped, Full | 25.0 fps, 0 dropped, Full |
| hevc_2160p30 (HEVC 4K30) | hw | 30.0 fps, 0 dropped, **Half** | 30.0 fps, 0 dropped, Full |
| screen_1080p60 (H.264) | hw | 57.2 fps, 18 dropped, **Half** | 58.5-60.0 fps, 0-9 dropped, Full |
| screen_1440p60 (H.264) | hw | 54.7 fps, 35 dropped, **Quarter (250x140 px)** | 51-60 fps, 0-54 dropped, Full |
| screen_1080p60 | sw | 52.5 fps, 45 dropped, **Quarter** | 60.0 fps, 0 dropped, Full (46.2 fps before the decode-ahead) |
| screen_1440p60 | sw | 26.3 fps, 215 dropped, **Quarter** | 49.3 fps, 65 dropped, Full (21.7 fps before the decode-ahead) |

Stage times after (ms per frame, hw decode): decode 0.0-0.7 (hidden by the decode-ahead; was
1.2-3.6), upload 0.5-2.3, composite 0.1-1.5, output 9-16 (llvmpipe executes the GL work at the
flush; on a GPU this is ~0).

`seekWhilePlaying` (hw decode; latency until a frame within 0.5 s of the target is on screen):

| File | Click, before | Click, after | Drag (40 moves): frames shown before → after | Last drag position on screen, before → after |
|---|---|---|---|---|
| perf_1080p | 75 ms (max 101) | 72-78 ms (max 111) | 0 → 24-40 | 0 → 0-23 ms |
| screen_1080p60 | 118 ms (max 161) | 70-87 ms (max 172) | 0 → 12-19 | 73 → 21-30 ms |
| hevc_2160p30 | 136 ms (max 199) | 101-114 ms (max 187) | 0 → 7-13 | 97 → 46-96 ms |

Before, the picture froze for the whole drag (every move restarted the sound device and waited
for the old look-ahead); with a real sound server the restart cost was higher still.

## Tests

`tests/playback/test_viewerpanel.cpp` (offscreen, raster path): program frames reach the
picture, header/timecode/scrub/marks, zoom and quality boxes, source clip loading and its
I/O/Home/Shift+I keys, marks remembered per clip, transport routing to the active viewer,
loop toggle, `kkProgramFocus`, full-screen viewer, F9 inserting the marked source range.
`tests/app/test_glviewer.cpp` checks real GL pixels (run under xvfb).

## How to extend

* **A transport button**: `addButton(bar, "<action id>", icon, tr("Tip"), handler)` in the
  ViewerPanel constructor; the handler should call `activate()` and act on `engine()`.
* **An overlay** (timecode burn-in, a new safe-area shape): a transparent child widget of the
  surface like `GuideOverlay` (both surface kinds), mapping frame px through
  `pictureRect()`; a new safe-area or aspect entry is a row in `ViewerOverlaySettings`
  (option, QSettings key, action), the action in `StandardActions.cpp` and the keymap script,
  and its drawing in `GuideOverlay::paintSafeAreas()`. Anything that must never be exported
  stays out of `src/render`; picture grabs call `GuideOverlay::setSuppressed()`.
* **Snapping another gesture to guides**: ask `GuideOverlay::snapTargets()` for the axis and
  use `guides::snapOffset()` (core/ViewerGuides.h) with a threshold in frame px.
* **Panning when zoomed**: keep an offset in `ViewerPicture` and add it in `pictureRect()`;
  middle-drag in `ViewerPanel::eventFilter()`.

## Known gaps

* TODO(viewer): no panning when zoomed in (the picture stays centred).
* TODO(viewer): dragging a source range shows a grab of the widget (the raster path draws
  the frame; on GL it is the widget's last frame).
* TODO(viewer): no A/B comparison.
* TODO(viewer): mask points do not snap to guides yet (spec 41.17).
