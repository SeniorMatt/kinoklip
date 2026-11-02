# Kinoklip

[![Download here](https://img.shields.io/badge/Download-here-3daee9?style=for-the-badge)](https://github.com/SeniorMatt/kinoklip/releases)

A non-linear video editor written from scratch in C++20 with Qt 6 Widgets and FFmpeg. The Edit
page layout follows the familiar professional NLE arrangement (media pool, source and timeline
viewers, inspector, multi-track timeline, page bar) with common industry shortcuts.
"Kinoklip" is a placeholder name.

- **Plan & architecture:** [plan.md](plan.md)
- **Coding style (read before contributing):** [docs/coding-style.md](docs/coding-style.md)

## Build

### Primary: the Arch Linux container (Qt 6.12, KDE Frameworks 6, Breeze)

The real KDE Breeze look (Breeze widget style, KDE colour schemes, Breeze icons; plan.md 4)
needs Qt >= 6.6 and KDE Frameworks 6, so the main toolchain is the podman container `kk-arch`
(Arch Linux: latest Qt 6, KF6, Breeze, FFmpeg, clang; the repository is mounted at the same
path). Packages it needs: `base-devel cmake ninja qt6-base qt6-multimedia qt6-svg
qt6-multimedia-ffmpeg ffmpeg kcolorscheme kiconthemes breeze breeze-icons libheif libraw
patchelf zstd xorg-server-xvfb clang`.

```sh
podman exec -w "$PWD" kk-arch sh -c 'cmake -S . -B build-arch -G Ninja && cmake --build build-arch -j4'
podman exec -w "$PWD" kk-arch sh -c 'QT_QPA_PLATFORM=offscreen ctest --test-dir build-arch -j4'
podman exec -w "$PWD" kk-arch ./build-arch/kinoklip [project.kkproj]     # needs a display, see below
```
CMake reports `KDE Frameworks 6 (Breeze colour schemes + icons): ON`. Release bundle
(`dist/Kinoklip-x86_64.run`) from the container: see [packaging/linux/readme.md](packaging/linux/readme.md).

### Fallback: host toolchain without KF6 (e.g. Linux Mint 22: Qt 6.4.2, FFmpeg 6.1)

Requirements: CMake >= 3.24, Ninja, a C++20 compiler, Qt 6.4+ (Core, Gui, Widgets, Multimedia,
Svg, OpenGL, OpenGLWidgets, Concurrent, Test), FFmpeg 6 development libraries, pkg-config.
Without KF6 the app uses Fusion (or a host Breeze) with palettes read from the bundled KDE
colour scheme files, and Breeze icons only if the host has them installed.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo   # -DKK_USE_KF6=OFF forces the fallback
cmake --build build -j4
ctest --test-dir build --output-on-failure      # tests run with QT_QPA_PLATFORM=offscreen
./build/kinoklip [project.kkproj]
```

Useful options:
```sh
./build/kinoklip --reset-layout                     # ignore the saved window layout
./build/kinoklip --color-scheme BreezeLight         # colour scheme for this run (or "system")
./build/kinoklip --widget-style Fusion              # widget style for this run
QT_QPA_PLATFORM=offscreen ./build/kinoklip --screenshot edit out.png [project.kkproj]
#   targets: media edit deliver preferences keyboard inspector mediapool menu-color-theme menu-style
KK_NO_OPENGL=1 ./build/kinoklip                     # force the raster viewer fallback
```
Format before committing: `scripts/format.sh` (clang-format, `--check` for CI).

## Directory map

| Path | What |
|---|---|
| `src/main.cpp` | entry point: OpenGL setup, model objects, command-line modes, main window |
| `src/app/` | main window, pages (Edit, Media), theme (Breeze style/colour schemes/icons), project/autosave controllers, CLI |
| `src/platform/` | OS integration helpers (child processes get the user's environment back when bundled) |
| `src/core/` | data model, EditOps, snapshot undo, JSON project files, effect catalogue, preferences |
| `src/media/` | FFmpeg probe/decoders/caches (`media::probe()` is the shared contract) |
| `src/ui/common/` | AppContext, ActionRegistry + standard actions, PanelFrame, Page, icons, timecode |
| `src/ui/mediapool/` | media pool, media browser, metadata panels |
| `src/ui/timeline/` | timeline panel |
| `src/ui/viewer/` | source/timeline viewers (OpenGL surface + raster fallback) |
| `src/ui/inspector/` | clip inspector |
| `src/ui/effects/` | effects library |
| `src/ui/deliver/` | Deliver (render) page |
| `resources/` | icons, default keymap, KDE colour schemes (fallback build), `.qrc` |
| `tests/<area>/test_*.cpp` | QtTest unit/smoke tests (each file is its own executable) |
| `docs/` | coding style and other docs |

Each module directory has a `readme.md` with its responsibilities and a **How to extend** section.

## License
GPL-3.0-or-later (see the SPDX headers; the AppImage bundles GPL FFmpeg components).
