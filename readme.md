# Kinoklip

A non-linear video editor written from scratch in C++20 with Qt 6 Widgets and FFmpeg. The Edit
page layout follows the familiar professional NLE arrangement (media pool, source and timeline
viewers, inspector, multi-track timeline, page bar) with common industry shortcuts.
"Kinoklip" is a placeholder name.

- **Plan & architecture:** [plan.md](plan.md)
- **Coding style (read before contributing):** [docs/coding-style.md](docs/coding-style.md)

## Build

Requirements: CMake ≥ 3.24, Ninja, a C++20 compiler, Qt 6.4+ (Core, Gui, Widgets, Multimedia,
Svg, OpenGL, OpenGLWidgets, Concurrent, Test), FFmpeg 6 development libraries, pkg-config.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
ctest --test-dir build --output-on-failure      # tests run with QT_QPA_PLATFORM=offscreen
./build/cutwise [project.kkproj]
```

Useful options:
```sh
./build/cutwise --reset-layout                      # ignore the saved window layout
QT_QPA_PLATFORM=offscreen ./build/cutwise --screenshot edit out.png [project.kkproj]
KK_NO_OPENGL=1 ./build/cutwise                      # force the raster viewer fallback
```
Format before committing: `scripts/format.sh` (clang-format, `--check` for CI).

## Directory map

| Path | What |
|---|---|
| `src/main.cpp` | entry point: OpenGL setup, model objects, command-line modes, main window |
| `src/app/` | main window, pages (Edit, Media), theme, project/autosave controllers, CLI |
| `src/core/` | data model, EditOps, snapshot undo, JSON project files, effect catalogue, preferences |
| `src/media/` | FFmpeg probe/decoders/caches (`media::probe()` is the shared contract) |
| `src/ui/common/` | AppContext, ActionRegistry + standard actions, PanelFrame, Page, icons, timecode |
| `src/ui/mediapool/` | media pool, media browser, metadata panels |
| `src/ui/timeline/` | timeline panel |
| `src/ui/viewer/` | source/timeline viewers (OpenGL surface + raster fallback) |
| `src/ui/inspector/` | clip inspector |
| `src/ui/effects/` | effects library |
| `src/ui/deliver/` | Deliver (render) page |
| `resources/` | icons, default keymap, `.qrc` |
| `tests/<area>/test_*.cpp` | QtTest unit/smoke tests (each file is its own executable) |
| `docs/` | coding style and other docs |

Each module directory has a `readme.md` with its responsibilities and a **How to extend** section.

## License
GPL-3.0-or-later (see the SPDX headers; the AppImage bundles GPL FFmpeg components).
