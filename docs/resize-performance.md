# Live window resizing

Owner reports: "the window flickers when you resize it on KDE Plasma" (alpha.3), then "dragging
the window [edge or corner] feels bad, it snaps to some values and looks stuttery", with a
screencast (0.3.4 bundle, Plasma Wayland): during the drag the content is often cut or has a
black band next to the new window frame, then catches up.

## How it was measured

- `kk.ui.resize` logging category (`src/ui/common/ResizeProfiler.h`): per size the window gets,
  the time from the resize event to the end of the last paint + flush at that size (the commit
  the compositor waits for), and a summary per drag. `kk.ui.resize.events` logs every resize /
  expose / update request with its start time. Checked against `WAYLAND_DEBUG=client`: the
  `wl_surface.commit` of each size comes at the end of the logged step.
- `test_resizebench sweep`: the Edit page resized 1280x720 -> 1920x1080 in 8 px steps at 60 Hz
  (dual viewers, single viewer, a clip loaded, Keyframes docked), offscreen in ctest and under
  Xvfb (X11 with the OpenGL viewers). Also counts widget resize events per step, resize events of
  hidden pages and the engine re-renders.
- `scripts/resize_sweep.sh`: a real interactive resize by the compositor (KWin: Meta + right drag
  through `kk_fakeinput`; weston: a press that starts `QWindow::startSystemResize()`, then
  `xdotool` moves), 80 steps of 8 px at about 60 Hz. With `WAYLAND_DEBUG=client` and
  `KK_SWEEP_LOG`, `scripts/wayland_resize_trace.py` reads the trace: configures skipped, EGL
  frames per size, frames committed at the previous size.

No GPU in the container: OpenGL is Mesa llvmpipe, so absolute numbers of the OpenGL path are
10x a real GPU; the counts (frames per size, sizes skipped, frames at the wrong size) are not.

## What was found

1. **Wayland, OpenGL composition (the viewers are QOpenGLWidgets): 2.5 full-window frames per
   size, the first often at the previous size.** Qt applies a configure as a resize event that
   repaints and swaps at once, then an expose that repaints and swaps again, then often an update
   request. In the trace the first frame's EGL buffer frequently has the previous size: that is
   the screencast's cut content / black band. With swap interval 1 (our default format) Qt makes
   every swap wait on the GUI thread for the compositor's frame callback, so the wrong frame
   stays on screen for a refresh, each size costs several refresh periods and the compositor
   coalesces the pointer's sizes (the "snaps").
2. **Edit page laid out twice per step.** `EditPage::resizeEvent()` re-applied the default
   column widths with `QSplitter::setSizes()` after the top row had already been laid out for
   the new size: both viewers (and their QOpenGLWidget FBOs), the media pool and the Inspector
   were resized twice per step.
3. Not a problem: hidden pages get no resize events (QStackedLayout only resizes the current
   page), the viewers re-render the picture only once per drag (`kResizeSettleMs`, the last frame
   is drawn scaled meanwhile, so the picture follows the window), nothing is saved or logged per
   step. The widget paint of a step is 4-6 ms offscreen (Fusion), 5-10 ms with Breeze
   (QToolButton, QComboBox paints dominate).
4. KWin's virtual backend (the only KWin that runs in the container) sends no frame callbacks:
   Qt then marks the window unexposed and does not repaint the OpenGL window during an
   interactive resize at all. Its numbers are not representative for the OpenGL path; weston was
   used for that.

## What changed

- On Wayland the default swap interval is 0 (`liveresize::adjustSwapInterval()`, from `main()`;
  `KK_SWAP_INTERVAL` overrides). Swaps no longer block on frame callbacks; Qt still paces updates
  by them and Wayland never tears; playback is timer-driven and unchanged.
- The Edit page applies its default column widths from an event filter on the top row, before
  `QSplitter::resizeEvent()`: one layout pass per step.

Tried and reverted: dropping the expose after the resize-event frame (it is the frame at the
right size), moving the paint from the resize event to the expose (the expose's frame then has
the previous size instead), flushing pending layout requests in the resize event (no change).

## Numbers

weston (nested, llvmpipe), Edit page dual viewers, 80 configures of 8 px:

| | sizes shown | ms per size | EGL frames per size | wrong-size frame shown for |
|---|---|---|---|---|
| before (swap interval 1) | 41 of 80 | 53 | 2.5 | 13.5 ms (median) |
| after (swap interval 0) | 50 of 80 | 38 | 2.5 | 8.6 ms |

On a GPU the composition is a few ms instead of 10-20, so with interval 1 a size costs up to
three refresh periods of waiting (about 50 ms, 20 sizes per second), with interval 0 about the
widget paint plus a few ms.

`test_resizebench sweep`, offscreen (raster), per step resize -> frame, median:

| | dual | single | clip loaded | Keyframes docked |
|---|---|---|---|---|
| before | 4.4 ms, viewers laid out 2x | 4.1 ms, 2x, 64 widget resizes | 5.5 ms | 5.0 ms |
| after | 4.5 ms, 1x | 4.2 ms, 1x, 57 widget resizes | 5.5 ms | 5.1 ms |

Xvfb (X11, OpenGL viewers on llvmpipe): 26 / 22 / 23 / 25 ms before and after (the OpenGL
composition of the whole window dominates; one frame per step on X11). The double layout pass
costs well under a millisecond, but it resized the viewers' FBOs twice per step.

KWin virtual backend, raster viewers (`KK_NO_OPENGL=1`,
`QT_WAYLAND_FRAME_CALLBACK_TIMEOUT=60000`): 80 of 80 sizes shown, 10 ms per step.

## Checking on a real Plasma session

1. Start from a terminal and drag the window corner for a few seconds:
   `QT_LOGGING_RULES=kk.ui.resize.debug=true <kinoklip> 2>&1 | grep kk.ui.resize` (the bundle's launcher)
   Each drag ends with `resize gesture: N steps, frame median ... ms / p90 / max, K over 16 ms`.
2. The same with the old swap interval for comparison: `KK_SWAP_INTERVAL=1` in front.
3. For the frame sizes: `WAYLAND_DEBUG=client <kinoklip> 2> trace.log`, drag, quit,
   then `scripts/wayland_resize_trace.py trace.log`: sizes skipped, frames per size, how many sizes
   first showed a frame at the previous size and for how long.
