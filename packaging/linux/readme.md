# Linux packaging

Produces one file the user can double-click -- `dist/Kinoklip-x86_64.run` -- plus a portable
folder and an archive of it. Built only from distribution tools (`ldd`, `readelf`,
`patchelf`, `tar`, `zstd`); no linuxdeploy/appimagetool (downloading them is not allowed in
the build environment). Primary target: ThinkPad T14s Gen 2 (Iris Xe), KDE Linux (Arch
based, Wayland). **Build host: the Arch Linux container `kk-arch`** (Qt 6.12, KDE Frameworks
6.31, Breeze 6.8.2, FFmpeg 9, glibc 2.44, GCC 16), so the bundle carries the real Breeze look
(plan.md 4): the Breeze style plugin, KDE's icon engine, the Breeze icons and the KDE colour
schemes. Verified to run on the build container and, from a clean environment, on Linux
Mint 22 (glibc 2.39, Breeze 5 icons on the host) with identical rendering.

## Build

```sh
# in the container (repository mounted at the same path):
podman exec -w "$PWD" kk-arch env KK_BUILD_DIR=build-arch scripts/package_linux.sh   # Release build + bundle + .tar.zst + .run
podman exec -w "$PWD" kk-arch scripts/check_bundle.sh                                 # verify library resolution
```

Options: `--no-build` (reuse `$KK_BUILD_DIR/kinoklip`), `--no-run`, `--no-tarball`.
Environment: `KK_BUILD_DIR`, `KK_JOBS`, `KK_COMPRESSION=zstd|xz|gzip`, `KK_ZSTD_LEVEL`,
`KK_QT_PLUGINS`. The script still works on a Debian/Ubuntu host (Qt 6.4, no KF6: no Breeze
parts, licences via dpkg) as a fallback.

Packages the container needs beyond the build dependencies: `patchelf zstd file` (licences
are collected with `pacman -Qo/-Qi` and `/usr/share/licenses`).

| Output | Size (v0.1.0, container build) | What |
|---|---|---|
| `dist/kinoklip-x86_64/` | 322 MB | portable folder, start `./AppRun` |
| `dist/kinoklip-x86_64.tar.zst` | 90 MB | the folder as an archive |
| `dist/Kinoklip-x86_64.run` | 90 MB | self-extracting single file |

## Files in this directory

| File | Purpose |
|---|---|
| `excludelist.txt` | which libraries come from the host (`host`), are only a fallback (`fallback`) or a fallback that also replaces an older host copy (`newer`), with reasons |
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
  usr/lib/*.so*                       also KDE Frameworks (KColorScheme, KIconThemes, KConfig,
                                      KWidgetsAddons, KGuiAddons, KArchive, KI18n,
                                      libKF6BreezeIcons with the icons compiled in)
  usr/lib/qt/*.so*                    Qt 6.12 libraries and libraries only Qt needs (ICU, ...)
  usr/lib/qt/qt-version.txt           Qt version built against (minimum for host-Qt mode)
  usr/lib/qt/plugins/                 platforms (wayland: libqwayland.so from Qt 6.10 on,
                                      wayland-egl/-generic before; xcb, offscreen, minimal),
                                      wayland shell/decoration/graphics integration,
                                      xcbglintegrations, imageformats, iconengines (svg),
                                      multimedia (FFmpeg backend), platforminputcontexts,
                                      styles/breeze6.so (Breeze style; KF6Style, KF6WindowSystem
                                      ... land in usr/lib/qt), kiconthemes6/iconengines
                                      (KIconEngine), kf6/kwindowsystem (X11/Wayland plugins)
  usr/lib/libheif/plugins/            libheif decoder plugins (dav1d, ffmpeg, j2k, rav1e, svt)
  usr/lib/glibc/                      the build host's glibc + glibc-required.txt (see below)
  usr/lib/qt/host-qt-requirements.txt Qt modules needed when running on a host Qt (opt-in)
  usr/lib/fallback/                   prefer-host libraries (libvdpau, xcb-util, ...) and
                                      "newer" ones (libstdc++, libgcc_s, libva, fontconfig,
                                      wayland-client/-cursor) + newer.txt (their version tags)
  usr/lib/fallback-active/            symlinks to the fallbacks the host lacks (made by AppRun)
  usr/share/color-schemes/            KDE colour schemes (BreezeDark, BreezeLight, BreezeClassic)
  usr/share/icons/breeze/             Breeze icon theme (wins over an older host Breeze)
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
FFmpeg), platform themes (GTK3 would drag in GTK; the KDE platform theme would make the
look host-dependent: the app sets style, colours and icons itself), `kstyle_config`, TLS/SQL/print
plugins. `qt.conf` is deliberately not used: Qt reads it from the binary's directory in
*every* mode, which would break the host-Qt mode; `QT_PLUGIN_PATH` does the same job.

## What comes from the host, and why

Full list with comments: `excludelist.txt`. Summary:

- **glibc** (libc, libm, ld-linux, ...): the host's, when it is new enough. An Arch build
  references the newest libm symbol versions (`sqrtf@GLIBC_2.43`, `cosh@GLIBC_2.44`, from
  FFmpeg, Qt, codecs), so on older hosts (Ubuntu 24.04 / Mint 22: 2.39) AppRun starts the
  binary through the bundled loader instead: `usr/lib/glibc/ld-linux-x86-64.so.2
  --library-path usr/lib/glibc:... usr/bin/kinoklip` (only when `getconf GNU_LIBC_VERSION`
  is older than `usr/lib/glibc/glibc-required.txt`). glibc is backwards compatible, so host
  GPU drivers and other host libraries load fine; ld.so options are not inherited, so child
  processes use the host glibc. `/proc/self/exe` is then the loader (the app does not use
  `applicationDirPath()`; `KINOKLIP_APPDIR` is set). Locale data comes from the host.
- **libstdc++/libgcc_s**: "newer" fallbacks: GCC 16's runtime (GLIBCXX_3.4.36) is used when
  the host's is older (Mint 22: 3.4.33); a host with the same or newer GCC runtime (KDE Linux)
  keeps its own, so host Mesa/LLVM never get an older one than they were built for.
- **GPU stack** (libGL/EGL/GLX/OpenGL/GLdispatch, gbm, drm, vulkan, CUDA/NVIDIA): must match
  the installed driver. **libvdpau** is a "fallback" (used only if missing). **libva** is a
  "newer" fallback (Arch FFmpeg calls `vaMapBuffer2`, libva >= 2.21); when the bundled libva
  is used, AppRun sets `LIBVA_DRIVERS_PATH` to the host's driver dirs (unless the user set
  it), otherwise `LIBVA_DRIVERS_PATH` is never touched.
- **Wayland client libs, xkbcommon, X11/xcb core**: match the compositor / the host keymaps.
  libwayland-client/-cursor are "newer" fallbacks (KF6GuiAddons uses
  `wl_display_create_queue_with_name`, wayland >= 1.23; the protocol stays compatible).
  The xcb-util libraries (icccm, image, keysyms, render-util, util), libSM/ICE and
  xkbcommon-x11 are fallbacks, because a Wayland-only install may lack them and the xcb
  platform plugin needs them.
- **fontconfig/freetype/harfbuzz/graphite2**: host, so the host fonts and configuration are
  found; harfbuzz and graphite2 because host freetype links them. fontconfig is a "newer"
  fallback (pango/libass from Arch call `FcConfigSetDefaultSubstitute`, fontconfig 2.17); it
  reads the host's /etc/fonts.
- **Audio** (libpulse, alsa, pipewire), **dbus, systemd/udev, glib/gio**: talk to host daemons
  or are loaded by the host anyway (newer, backwards compatible).
- **zlib, zstd, xz, bzip2, lz4, brotli, expat, libffi, libpng**: shared with Mesa/LLVM/
  freetype/systemd. A bundled older copy would be loaded first and shadow the newer one those
  host libraries were built against.

`scripts/check_bundle.sh` proves that every direct dependency of every bundled ELF resolves
inside the bundle or to one of these, once normally and once with all fallbacks forced, and
prints the minimum host glibc without the bundled one plus the host libraries the target must
provide. Symbol-level gaps against an older host (what decided the "newer" entries above) are
found on that host with the bundled loader:

```sh
L=dist/kinoklip-x86_64/usr/lib
for f in dist/kinoklip-x86_64/usr/bin/kinoklip $(find $L/qt/plugins $L/libheif -name '*.so'); do
    env -i LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes \
        $L/glibc/ld-linux-x86-64.so.2 --library-path $L/glibc:$L/qt:$L/fallback-active "$f"
done | grep -E 'undefined symbol|not found'
```

## The launcher (`AppRun`)

1. Links the fallback libraries the host lacks into `usr/lib/fallback-active/` (looked up in
   the `ldconfig -p` cache, else the usual lib dirs), plus the "newer" ones whose host copy
   is older (highest `GLIBCXX_`/`GCC_` tag in the file, or the version in the real file name,
   against `usr/lib/fallback/newer.txt`). RUNPATH is searched before the system paths, so
   only those may be linked there. Needs a writable bundle (true for the `.run` cache and a
   user-unpacked folder; otherwise a warning is printed).
2. Chooses the Qt:
   - **Default: bundled Qt 6.12 + KF6 + Breeze.** `LD_LIBRARY_PATH=usr/lib/qt`,
     `QT_PLUGIN_PATH=usr/lib/qt/plugins` (host plugins are built for another Qt/KF6),
     `QT_QPA_PLATFORMTHEME=generic` and `QT_STYLE_OVERRIDE` unset, so neither a host platform
     theme nor Qt's built-in KDE theme can restyle the app: Breeze style, KDE colour scheme and
     Breeze icons come from the bundle and look identical on every system (the user changes
     them in Settings > Color Theme / Style). `QT_MEDIA_BACKEND=ffmpeg` (unless set).
   - **Opt-in: `KINOKLIP_HOST_QT=1`** uses the system Qt if it is Qt 6 and at least the
     version in `usr/lib/qt/qt-version.txt` (6.12), has every module in
     `host-qt-requirements.txt` from the same release, and has a wayland or xcb platform
     plugin; otherwise it falls back to the bundled Qt. Nothing Qt-related is set, so the host
     Qt finds its own plugins (incl. the host's Breeze). FFmpeg, codecs and the KF6 libraries
     still come from the bundle.
   - `KINOKLIP_BUNDLED_QT=1` forces the bundled Qt (wins over `KINOKLIP_HOST_QT`).
   The choice is logged to stderr (`KINOKLIP_QUIET=1` silences it).
3. `XDG_DATA_DIRS=usr/share:<previous or /usr/local/share:/usr/share>` (KColorSchemeManager
   finds the colour schemes, KIconLoader the Breeze icons there),
   `LIBHEIF_PLUGIN_PATH=usr/lib/libheif/plugins`, `QT_QPA_PLATFORM=wayland;xcb` unless the
   user set it; exports `KINOKLIP_APPDIR`.
4. Every variable it changes is recorded first: `KINOKLIP_ORIG_VARS` lists the names and
   `KINOKLIP_ORIG_<NAME>` holds the original value (absent = was unset). The app
   (`src/platform/ChildProcessEnv.h`: `childEnvironment()`, `prepareChildProcess()`,
   `openUrl()`) restores exactly that for programs it starts (xdg-open, the file manager,
   ...), so they never see the bundle's Qt, plugins or data dirs.
5. Execs `usr/bin/kinoklip` with all arguments -- through the bundled glibc loader if the
   host glibc is too old (see above; `KINOKLIP_FORCE_GLIBC=1|0` test hook).

`AppRun --install-desktop-entry` writes the desktop entry, icon and MIME type to
`${XDG_DATA_HOME:-~/.local/share}` with `Exec=` pointing at the `.run` file (or at AppRun for
the folder), then refreshes the MIME/desktop/KDE caches if the tools exist.
`--uninstall-desktop-entry` removes them.

Every normal start also does this automatically (quietly) when
`~/.local/share/applications/org.kinoklip.Kinoklip.desktop` is missing or differs (first start,
the `.run` moved, a new version): Qt registers the app id with the XDG desktop portal at
start-up and logs `Failed to register with host portal ... App info not found` without it, and
Plasma uses the entry for the Wayland window icon. Normal starts only compare one small file.
`--uninstall-desktop-entry` writes `~/.config/kinoklip/no-desktop-entry` (opt-out, removed again
by `--install-desktop-entry`); `KINOKLIP_NO_DESKTOP_ENTRY=1` skips it for one run.

`scripts/check_bundle.sh` fails when a Wayland or X11 platform plugin, the xdg-shell
integration or the Wayland EGL client integration is missing (alpha.1 shipped without a
Wayland platform plugin because Qt 6.10 renamed it). Manual Wayland check in the container:
`weston --backend=headless --socket=kk-wl &` then
`WAYLAND_DISPLAY=kk-wl QT_QPA_PLATFORM=wayland dist/kinoklip-x86_64/AppRun --screenshot edit /tmp/wl.png`.

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

## Verification

```sh
# in the container
podman exec -w "$PWD" kk-arch scripts/check_bundle.sh
podman exec -w "$PWD" kk-arch env -i HOME=/root PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen \
    dist/kinoklip-x86_64/AppRun --screenshot edit /tmp/edit.png
podman exec -w "$PWD" kk-arch env -i HOME=/root PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen \
    KINOKLIP_FORCE_GLIBC=1 dist/kinoklip-x86_64/AppRun --screenshot edit /tmp/edit.png
# on another distribution (done on Linux Mint 22: glibc 2.39, GCC 13 runtime, Breeze 5 icons)
env -i HOME=$HOME PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen \
    dist/Kinoklip-x86_64.run --color-scheme BreezeLight --screenshot preferences /tmp/prefs.png
```
On Mint the launcher reports `using bundled fallbacks: libfontconfig.so.1 libstdc++.so.6
libva-drm.so.2 libva-x11.so.2 libva.so.2 libwayland-client.so.0 libwayland-cursor.so.0` and
`using the bundled glibc (host: 2.39, bundle needs: 2.44)`; the screenshots are pixel-identical
in style and icons to the container's, and CUDA/Vulkan decoding is detected with the host's
NVIDIA driver.

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
- Programs the app starts get the original environment back only where the app goes through
  `src/platform/ChildProcessEnv` (currently: reveal in file manager). New `QProcess` /
  `QDesktopServices` call sites must use it too.
- The bundled Qt does not use the KDE platform theme: file dialogs are Qt's own widgets (no
  portal/KIO dialogs). Intended (identical look everywhere).
- The Wayland app_id is `org.kinoklip.Kinoklip` (`setDesktopFileName`), matching the desktop
  file name; on X11 KWin matches through the same desktop file name, and
  `StartupWMClass=kinoklip` still matches the WM_CLASS instance (binary name).
- Bundled-glibc mode (older hosts only): NSS modules and gconv come from the host's glibc
  version and may not load into the newer glibc (user names via NSS, iconv charsets); Qt uses
  neither for normal operation. KDE Linux / current Arch never use this mode.
- Host libraries newer than ours with the same soname are shared (e.g. bundled `libwebp`,
  `libjpeg` vs. the host's in host-Qt mode). Not seen in tests; watch for "undefined symbol"
  warnings from host image plugins in host-Qt mode.
