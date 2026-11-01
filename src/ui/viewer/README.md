# src/ui/viewer — source and timeline viewers

**Status: placeholder from the foundation agent; the playback agent owns this module.**

| File | What |
|---|---|
| `ViewerPanel.h` | panel = title bar with timecode + `ViewerSurface` + transport bar; `Mode::Source` / `Mode::Timeline` |
| `ViewerSurface.h` | interface of the picture area; `ViewerSurface::create()` picks GL or raster; `ViewerPicture` = shared state + letterbox maths (+ QPainter drawing for raster) |
| `GlViewerSurface.h` | `QOpenGLWidget`, GLSL 330 core: one persistent texture updated with `glTexSubImage2D` straight from ARGB32 memory (`GL_BGRA`, no conversion, no per-frame allocation), scissored letterbox, overlay label for the empty state |
| `RasterViewerSurface.h` | `QWidget` fallback (offscreen screenshots, CI, no GPU, `CW_NO_OPENGL=1`) |

Timeline-mode transport buttons trigger the global `transport.*` actions; Source-mode buttons
emit `playPauseRequested()`, `stepRequested(int)`, ... instead. The timeline viewer's timecode
already follows `Timeline::playheadChanged`.

## How to extend
- Zero-copy GPU display: add a `setTexture(GLuint, QSize)` path to `GlViewerSurface` that draws
  the GPU compositor's output texture directly (contexts are shared:
  `Qt::AA_ShareOpenGLContexts` is set in `main.cpp`). Keep `RasterViewerSurface` working for
  headless runs.
- `tests/app/test_glviewer.cpp` checks real pixels; its GL case skips offscreen — run
  `QT_QPA_PLATFORM=xcb xvfb-run -a ./build/test_glviewer` to exercise it.
- Feed frames with `ViewerPanel::showFrame(QImage)` (CPU path) and use `pictureSize()` to choose
  the decode/render size.
