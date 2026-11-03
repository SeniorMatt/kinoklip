// SPDX-License-Identifier: GPL-3.0-or-later
// LiveResize — makes an interactive window resize cost one frame per size on Wayland.
#pragma once

class QWidget;

namespace kk::liveresize {

/// On Wayland, sets the default surface format's swap interval to 0 (call after QApplication
/// exists, before the first window is shown; no effect on other platforms).
///
/// With swap interval 1 Qt's Wayland plugin makes every eglSwapBuffers() wait on the GUI thread
/// for the compositor's frame callback of the previous frame (up to 100 ms). A window with
/// QOpenGLWidgets (the viewers) is composed with OpenGL as a whole, so every frame of the main
/// window is such a swap; while the user resizes, Qt sends 2-3 frames per new size (the
/// configure, an expose, an update request: WAYLAND_DEBUG trace in docs/resize-performance.md),
/// so each size cost up to three refresh periods of blocking and the compositor's configure
/// events piled up and were skipped: the window followed the pointer in jumps. With interval
/// 0 the swaps do not block; Qt still paces updates by frame callbacks (QWindow::requestUpdate),
/// and a Wayland compositor never tears, so playback is unchanged (it is timer-driven, see
/// src/playback/readme.md). KK_SWAP_INTERVAL overrides the value on any platform.
void adjustSwapInterval();

/// Installs the live-resize helpers on the top-level @p window: on Wayland, the expose event
/// Qt sends right after it has painted and committed a new size (a second full-window frame,
/// identical to the first) is dropped. No effect on other platforms (unless @p force: tests),
/// or with KK_KEEP_RESIZE_EXPOSE set.
void install(QWidget* window, bool force = false);

} // namespace kk::liveresize
