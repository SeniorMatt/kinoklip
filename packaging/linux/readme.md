# Linux packaging

Produces one file the user can double-click -- `dist/Kinoklip-x86_64.run` -- plus a portable
folder and an archive of it. Built only from distribution tools (`ldd`, `readelf`,
`patchelf`, `tar`, `zstd`); no linuxdeploy/appimagetool (downloading them is not allowed in
the build environment). Primary target: ThinkPad T14s Gen 2 (Iris Xe), KDE Linux (Arch
based, Wayland). Build host: Ubuntu 24.04 / Linux Mint 22 (glibc 2.39, GCC 13, Qt 6.4.2,
FFmpeg 6.1).

## Build

```sh
scripts/package_linux.sh          # Release build + bundle + .tar.zst + .run (about 2 min)
scripts/check_bundle.sh           # verify library resolution (exit code 1 on problems)
```

Options: `--no-build` (reuse `build/kinoklip`), `--no-run`, `--no-tarball`. Environment:
`KK_BUILD_DIR`, `KK_JOBS`, `KK_COMPRESSION=zstd|xz|gzip`, `KK_ZSTD_LEVEL`, `KK_QT_PLUGINS`.

| Output | Size (v0.1.0) | What |
|---|---|---|
| `dist/kinoklip-x86_64/` | 192 MB | portable folder, start `./AppRun` |
| `dist/kinoklip-x86_64.tar.zst` | 60 MB | the folder as an archive |
| `dist/Kinoklip-x86_64.run` | 60 MB | self-extracting single file |

## Files in this directory

| File | Purpose |
|---|---|
| `excludelist.txt` | which libraries come from the host (`host`) or are only a fallback (`fallback`), with reasons |
| `apprun.sh` | the launcher, copied to `<bundle>/AppRun` |
| `runheader.sh` | shell header of the `.run` file (template filled by `package_linux.sh`) |
| `org.kinoklip.Kinoklip.desktop` | desktop entry (validated with `desktop-file-validate`) |
| `org.kinoklip.Kinoklip.xml` | MIME type `application/x-kinoklip-project` for `*.kkproj` |
| `icons/org.kinoklip.Kinoklip.svg` | app icon (original: film frame, "K" with a clapper-stick arm) |

## Bundle layout (AppDir-shaped)

```
kinoklip-x86_64/
  AppRun                              launcher (POSIX sh)
  org.kinoklip.Kinoklip.desktop       desktop entry   } AppImage expects these
  org.kinoklip.Kinoklip.svg, .DirIcon icon            } at the top level
  usr/bin/kinoklip                    RUNPATH $ORIGIN/../lib:$ORIGIN/../lib/fallback-active
  usr/lib/*.so*                       FFmpeg + codecs (x264, x265, dav1d, aom, svt-av1, vpx,
                                      rav1e, opus, lame, vorbis, webp, jxl, ...) and their deps
  usr/lib/qt/*.so*                    Qt 6.4 libraries and libraries only Qt needs (ICU, ...)
  usr/lib/qt/plugins/                 platforms (wayland-egl, wayland-generic, xcb, offscreen,
                                      minimal), wayland shell/decoration/graphics integration,
                                      xcbglintegrations, imageformats, iconengines (svg),
                                      multimedia (FFmpeg backend), platforminputcontexts
  usr/lib/qt/host-qt-requirements.txt Qt modules needed when running on a host Qt (opt-in)
  usr/lib/fallback/                   prefer-host libraries (libva, libvdpau, xcb-util, ...)
  usr/lib/fallback-active/            symlinks to the fallbacks the host lacks (made by AppRun)
  usr/share/applications, icons/hicolor/scalable/apps, mime/packages
  usr/share/doc/kinoklip/             bundled-libraries.txt (soname, package, version) and
                                      licenses/<package>.copyright for every bundled library
```

All libraries are found through RUNPATH (set with patchelf on every bundled ELF), so the
bundle needs no `LD_LIBRARY_PATH` -- except to select the bundled Qt (see below). Qt is kept
in its own directory so that the binary's RUNPATH never points at it: with
`LD_LIBRARY_PATH=usr/lib/qt` the bundled Qt is used, without it the system Qt.

Not shipped: `egldeviceintegrations` and the eglfs/linuxfb/vnc platforms (only for running
without a desktop session), the GStreamer multimedia backend (the FFmpeg backend shares our
FFmpeg), platform themes (GTK3 would drag in GTK; the app styles itself), TLS/SQL/print
plugins. `qt.conf` is deliberately not used: Qt reads it from the binary's directory in
*every* mode, which would break the host-Qt mode; `QT_PLUGIN_PATH` does the same job.

## What comes from the host, and why

Full list with comments: `excludelist.txt`. Summary:

- **glibc** (libc, libm, ld-linux, ...): must match the kernel/loader. The bundle needs
  glibc >= 2.39 (the build host's); KDE Linux has a newer one.
- **libstdc++/libgcc_s**: not bundled. The host (GCC 15 on Arch) is newer and backwards
  compatible, and a bundled older copy would break host libraries such as Mesa/LLVM. Bundle
  them only if a target ever has an *older* GCC runtime than GCC 13.
- **GPU stack** (libGL/EGL/GLX/OpenGL/GLdispatch, gbm, drm, vulkan, CUDA/NVIDIA): must match
  the installed driver. **libva/libvdpau** are "fallback": the host copy (which loads the
  host's iHD VAAPI driver) is used when present; `LIBVA_DRIVERS_PATH` is never touched.
- **Wayland client libs, xkbcommon, X11/xcb core**: match the compositor / the host keymaps.
  The xcb-util libraries (icccm, image, keysyms, render-util, util), libSM/ICE and
  xkbcommon-x11 are fallbacks, because a Wayland-only install may lack them and the xcb
  platform plugin needs them.
- **fontconfig/freetype/harfbuzz/graphite2**: host, so the host fonts and configuration are
  found; harfbuzz and graphite2 because host freetype links them.
- **Audio** (libpulse, alsa, pipewire), **dbus, systemd/udev, glib/gio**: talk to host daemons
  or are loaded by the host anyway (newer, backwards compatible).
- **zlib, zstd, xz, bzip2, lz4, brotli, expat, libffi, libpng**: shared with Mesa/LLVM/
  freetype/systemd. A bundled older copy would be loaded first and shadow the newer one those
  host libraries were built against.

`scripts/check_bundle.sh` proves that every direct dependency of every bundled ELF resolves
inside the bundle or to one of these, once normally and once with all fallbacks forced, and
prints the host libraries the target must provide.

## The launcher (`AppRun`)

1. Links the fallback libraries the host lacks into `usr/lib/fallback-active/` (looked up in
   the `ldconfig -p` cache, else the usual lib dirs). RUNPATH is searched before the system
   paths, so only missing ones may be linked there. Needs a writable bundle (true for the
   `.run` cache and a user-unpacked folder; otherwise a warning is printed).
2. Chooses the Qt:
   - **Default: bundled Qt 6.4.** `LD_LIBRARY_PATH=usr/lib/qt`, `QT_PLUGIN_PATH=usr/lib/qt/plugins`
     (host plugins are built for a newer Qt and would crash Qt 6.4),
     `QT_QPA_PLATFORMTHEME=generic` and `QT_STYLE_OVERRIDE` unset, so neither a host platform
     theme nor Qt's built-in KDE theme can restyle the app: it looks identical on every system
     with its own style and palette. `QT_MEDIA_BACKEND=ffmpeg` (unless set).
   - **Opt-in: `KINOKLIP_HOST_QT=1`** uses the system Qt if it is Qt 6 >= 6.4, has every module
     in `host-qt-requirements.txt` from the same release, and has a wayland or xcb platform
     plugin; otherwise it falls back to the bundled Qt. Nothing Qt-related is set, so the host
     Qt finds its own plugins. FFmpeg and codecs still come from the bundle.
   - `KINOKLIP_BUNDLED_QT=1` forces the bundled Qt (wins over `KINOKLIP_HOST_QT`).
   The choice is logged to stderr (`KINOKLIP_QUIET=1` silences it).
3. `QT_QPA_PLATFORM=wayland;xcb` unless the user set it; exports `KINOKLIP_APPDIR`; execs
   `usr/bin/kinoklip` with all arguments.

`AppRun --install-desktop-entry` writes the desktop entry, icon and MIME type to
`${XDG_DATA_HOME:-~/.local/share}` with `Exec=` pointing at the `.run` file (or at AppRun for
the folder), then refreshes the MIME/desktop/KDE caches if the tools exist.
`--uninstall-desktop-entry` removes them.

Test hooks: `KINOKLIP_FORCE_FALLBACK=1` (use every fallback), `KINOKLIP_HOST_LIBDIRS=dir:dir`
(search only these dirs for host libraries, e.g. an empty dir to simulate a bare host).

## The single-file `.run`

`runheader.sh` (POSIX sh, starts with `#!/bin/sh`, mode 755) followed directly by the
`.tar.zst` payload. On start it unpacks once into
`${XDG_CACHE_HOME:-~/.cache}/kinoklip/<version>-<sha256 prefix of the payload>/`: into a
temporary directory first, then writes the marker `.kinoklip-complete` and renames the
directory into place, so an interrupted unpack is redone next time. Later starts only check
the marker. Then it execs `AppRun` with all arguments and
`KINOKLIP_RUN_PATH` set. Unpacking needs `zstd` + `tar` (present on every Arch system:
systemd depends on zstd) or `bsdtar`; errors are shown with `kdialog`/`notify-send` when there
is no terminal.

Flags: `--extract-only` (prints the bundle dir), `--uninstall-cache` (deletes all unpacked
versions), `--install-desktop-entry`, `--uninstall-desktop-entry`. The archive is reproducible
(sorted, fixed owner/mtime), so rebuilding unchanged content keeps the same cache directory.
Old versions are not deleted automatically (one may still be running); use `--uninstall-cache`.

## Verification (done on the build host)

```sh
scripts/check_bundle.sh
env -i HOME=$HOME PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen \
    dist/kinoklip-x86_64/AppRun --screenshot edit /tmp/edit.png
env -i HOME=$HOME PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen \
    dist/Kinoklip-x86_64.run --screenshot edit /tmp/edit.png
```

With `LD_DEBUG=libs` the loaded objects were confirmed: bundled mode loads Qt and plugins
only from the bundle; `KINOKLIP_HOST_QT=1` loads `/lib/x86_64-linux-gnu/libQt6Core.so.6` and
the host's platform plugin; with `KINOKLIP_HOST_LIBDIRS=<empty dir>` the host Qt is rejected
and libva/libvdpau come from `fallback-active/`.

## Making a real AppImage later

The folder is already AppDir-shaped (`AppRun`, top-level `.desktop`, icon, `.DirIcon`). With
`appimagetool` available:

```sh
scripts/package_linux.sh --no-run --no-tarball
ARCH=x86_64 appimagetool dist/kinoklip-x86_64 dist/Kinoklip-x86_64.AppImage
```

Caveats: an AppImage mounts read-only, so `fallback-active/` cannot be populated; either
make the launcher create it under `$XDG_RUNTIME_DIR` and add that path via `LD_LIBRARY_PATH`
or move the fallbacks to plain `usr/lib` for the AppImage. AppImage tooling prefers a PNG
`.DirIcon` (render the SVG to 256x256). The AppImage runtime also needs FUSE 2 on the host
(or `--appimage-extract-and-run`); the `.run` file avoids that dependency.

## Known risks on the target

- Dolphin asks "Execute / Open" for executable scripts on double-click (the `.run` is a
  shell script). Choose Execute, or use `--install-desktop-entry` once and start from the menu.
- `LD_LIBRARY_PATH`/`QT_PLUGIN_PATH` (bundled-Qt mode) are inherited by processes the app
  starts (e.g. `xdg-open` -> a KDE app linked to the newer host Qt). If such a child fails,
  the app should restore the environment for children (future `src/platform/` work).
- Bundled Qt 6.4 does not use the KDE platform theme: file dialogs are Qt's own widgets (no
  portal/KIO dialogs). Intended (identical look everywhere).
- The Wayland app_id is `kinoklip` (`setDesktopFileName` in `main.cpp`) while the desktop file
  is `org.kinoklip.Kinoklip.desktop`; `StartupWMClass=kinoklip` covers the match, but renaming
  the app_id to `org.kinoklip.Kinoklip` would be cleaner.
- Host libraries newer than ours with the same soname are shared (e.g. bundled `libwebp`,
  `libjpeg` vs. the host's in host-Qt mode). Not seen in tests; watch for "undefined symbol"
  warnings from host image plugins in host-Qt mode.
