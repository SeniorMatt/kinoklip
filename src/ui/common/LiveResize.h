// SPDX-License-Identifier: GPL-3.0-or-later
// LiveResize — the Wayland swap interval that lets the window content keep up while resizing.
#pragma once

namespace kk::liveresize {

/// On Wayland, sets the default surface format's swap interval to 0 (call after QApplication
/// exists, before the first window is shown; no effect on other platforms).
///
/// A window with QOpenGLWidgets (the viewers) is composed with OpenGL as a whole, so every
/// frame of the main window is an eglSwapBuffers(). With swap interval 1 Qt's Wayland plugin
/// makes each swap wait on the GUI thread for the compositor's frame callback of the previous
/// frame. While the user resizes, Qt sends 2.5 frames per new size (the resize event, the
/// expose after the configure, an update request; WAYLAND_DEBUG trace in
/// docs/resize-performance.md), and the first of them is often composed for the previous size
/// (its EGL buffer still has the old size: the content is cut or a black band shows next to
/// the new window frame, as in the 0.3.4 screencast). With interval 1 that wrong frame stays on
/// screen until the next frame callback and every size costs several refresh periods of
/// blocking, so configure events pile up and are skipped (the window follows the pointer in
/// jumps). With interval 0 the swaps do not block, the correct frame replaces the wrong one
/// within its render time (usually before the compositor's next repaint), and more sizes are
/// shown. Qt still paces updates by frame callbacks (QWindow::requestUpdate) and a Wayland
/// compositor never tears, so playback is unchanged (it is timer-driven, see
/// src/playback/readme.md). KK_SWAP_INTERVAL overrides the value on any platform.
void adjustSwapInterval();

} // namespace kk::liveresize
