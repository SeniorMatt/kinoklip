# Windows packaging (not implemented yet)

Plan for a Windows build of Kinoklip. Nothing here is automated yet; the code already follows
the portability rules in `docs/archive/plan.md` (Qt + FFmpeg + std only, platform code in `src/platform/`).

## 1. Toolchain
- Visual Studio 2022 (MSVC v143, C++20), CMake >= 3.24, Ninja (from the VS developer prompt).
- Qt 6.4.x for `msvc2019_64` (binary compatible with v143) via the Qt online installer, or
  vcpkg ports `qtbase`, `qtsvg`, `qtmultimedia`.
- FFmpeg 6.1 via vcpkg, manifest mode (`vcpkg.json` at the repo root):
  `ffmpeg[core,avcodec,avformat,avfilter,swscale,swresample,x264,x265,dav1d,aom,vpx,opus,mp3lame,webp,nvcodec,qsv,amf]`
  with triplet `x64-windows` (DLLs). x264/x265 make the build GPL, which matches the project
  licence (docs/archive/plan.md section 8).
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake`. On Windows,
  `CMakeLists.txt` must find FFmpeg without pkg-config (vcpkg's `FindFFMPEG` /
  `find_package(FFMPEG)`, or an `FFMPEG_ROOT` + `find_library` branch, as noted there).

## 2. Hardware acceleration
- Decode: D3D11VA (all vendors), NVDEC/CUDA, QSV on Intel. Encode: NVENC, QSV, AMF.
- OpenGL 3.3 core is available on all three vendors' Windows drivers; keep
  `QT_OPENGL=desktop` (never ANGLE/software) and add a startup check with a clear message.

## 3. Deployment
1. `cmake --install build --prefix stage` (the `install(TARGETS ...)` rule already exists).
2. `windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw stage/bin/kinoklip.exe`
   copies Qt DLLs and plugins (platforms/qwindows, styles/qwindowsvistastyle, imageformats,
   iconengines, multimedia/ffmpegmediaplugin).
3. Copy the vcpkg FFmpeg DLLs and their codec DLLs next to the exe (`vcpkg` installs them to
   `vcpkg_installed/x64-windows/bin`); verify with `dumpbin /dependents` or `Dependencies.exe`
   that nothing resolves outside the folder except Windows system DLLs.
4. Bundle the MSVC runtime (`vcruntime140*.dll`, `msvcp140*.dll`) app-locally or require the
   VC++ redistributable in the installer.
5. Icon: pack `resources/icons/app/kinoklip-<size>.png` into a multi-size `.ico`
   (16-256 px) and add an `.rc` file with icon + version info to the executable.

## 4. Installer and signing
- Installer: Inno Setup or NSIS (per-user install, Start menu entry, `.kkproj` file
  association, uninstaller); MSIX later for the Microsoft Store.
- Also ship a portable `.zip` of the staged folder.
- Sign the exe, DLLs we build and the installer with `signtool` (EV/OV certificate) to avoid
  SmartScreen warnings.

## 5. CI
- GitHub Actions `windows-2022` runner: cache vcpkg binaries, build, run `ctest`
  (`QT_QPA_PLATFORM=offscreen`), deploy, build the installer, upload artifacts.
