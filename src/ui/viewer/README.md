# src/ui/viewer — source and timeline viewers

**Status: placeholder from the foundation agent; the playback agent owns this module.**

| File | What |
|---|---|
| `ViewerPanel.h` | panel = title bar with timecode + `ViewerSurface` + transport bar; `Mode::Source` / `Mode::Timeline` |
| `ViewerSurface.h` | interface of the picture area; `ViewerSurface::create()` picks GL or raster; `ViewerPicture` holds the shared placeholder drawing |
| `GlViewerSurface.h` | `QOpenGLWidget` implementation (default when OpenGL 3.3 widgets work) |
| `RasterViewerSurface.h` | `QWidget` fallback (offscreen screenshots, CI, no GPU, `CW_NO_OPENGL=1`) |

Timeline-mode transport buttons trigger the global `transport.*` actions; Source-mode buttons
emit `playPauseRequested()`, `stepRequested(int)`, ... instead. The timeline viewer's timecode
already follows `Timeline::playheadChanged`.

## How to extend
- Real GPU display: implement `GlViewerSurface::initializeGL()/paintGL()` with a texture +
  shader blit, or draw the compositor's output texture directly (contexts are shared:
  `Qt::AA_ShareOpenGLContexts` is set in `main.cpp`). Keep `RasterViewerSurface` working for
  headless runs.
- Feed frames with `ViewerPanel::showFrame(QImage)` (CPU path) and use `pictureSize()` to choose
  the decode/render size.
