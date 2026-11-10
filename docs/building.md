# Building Kinoklip

## Primary: the Arch Linux container (Qt 6.12, KDE Frameworks 6, Breeze)

The real [KDE Breeze](https://invent.kde.org/plasma/breeze) look (Breeze widget style, KDE colour schemes, Breeze icons; [docs/archive/plan.md](archive/plan.md) section 4)
needs [Qt](https://www.qt.io) >= 6.6 and KDE Frameworks 6, so the main toolchain is the podman container `kk-arch`
(Arch Linux: latest Qt 6, KF6, Breeze, [FFmpeg](https://ffmpeg.org), clang; the repository is mounted at the same
path). Packages it needs: `base-devel cmake ninja qt6-base qt6-multimedia qt6-svg
qt6-multimedia-ffmpeg ffmpeg kcolorscheme kiconthemes breeze breeze-icons libheif libraw
patchelf zstd xorg-server-xvfb clang`.

```sh
podman exec -w "$PWD" kk-arch sh -c 'cmake -S . -B build-arch -G Ninja && cmake --build build-arch -j4'
podman exec -w "$PWD" kk-arch sh -c 'QT_QPA_PLATFORM=offscreen ctest --test-dir build-arch -j4'
podman exec -w "$PWD" kk-arch ./build-arch/kinoklip [project.kkproj]     # needs a display, see below
```
CMake reports `KDE Frameworks 6 (Breeze colour schemes + icons): ON`. Release AppImage
(`dist/Kinoklip-x86_64.AppImage`) from the container: see [packaging/linux/readme.md](../packaging/linux/readme.md).

## Fallback: host toolchain without KF6 (e.g. Linux Mint 22: Qt 6.4.2, FFmpeg 6.1)

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

### Command line (no window)

`kinoklip <command> --option value ...` runs one command of the command layer
(`src/app/commands/`) headless, on Qt's offscreen platform (no display needed), and exits.
`--json` prints the result as JSON; exit code 0 = done, 1 = the command failed, 2 = wrong command
line; messages and progress go to stderr. The window opens only when the first argument is not a
command (a project file, an option, nothing). Guide: [docs/guide/control-kinoklip-from-the-command-line.md](guide/control-kinoklip-from-the-command-line.md).
```sh
./build/kinoklip commands [--json]                  # every command (with --json: every option)
./build/kinoklip help clip add                      # options of one command
./build/kinoklip project new --project demo.kkproj --fps 25
./build/kinoklip media import --project demo.kkproj --file a.mp4
./build/kinoklip clip add --project demo.kkproj --media a.mp4
./build/kinoklip render --project demo.kkproj --output demo.mp4 --preset youtube_1080p
```
Commands: version; project new / info; media import / list / relink; timeline list / new /
select; clip list / add / move / trim / split / delete / set; marker add / list / delete;
transition add; export otio / edl; import otio; frame export / view; effect list;
transition list; render. `kinoklip mcp` serves the same commands to an AI assistant (MCP) on
stdin / stdout, see [docs/guide/control-kinoklip-from-an-ai-assistant.md](guide/control-kinoklip-from-an-ai-assistant.md). The older options above
(`--render`, `--render-frame`, `--screenshot`, ...) are unchanged. Tests: `test_commands` (the
layer) and `test_cli` (the built binary as a process).
