# Linux packaging

Produces **one release file, `dist/Kinoklip-x86_64.AppImage`** (type 2 AppImage with the static
runtime, ~100 MB), plus `Kinoklip-x86_64.AppImage.zsync` for delta updates and the AppDir it is
made from. The AppDir is assembled with distribution tools only (`ldd`, `readelf`, `patchelf`);
the AppImage is made with the official `appimagetool`, downloaded once at a pinned version and
checked against its SHA-256 (see "The AppImage" below). Primary target: ThinkPad T14s Gen 2
(Iris Xe), KDE Linux (Arch based, Wayland). **Build host: the Arch Linux container `kk-arch`**
(Qt 6.12, KDE Frameworks 6.31, Breeze 6.8.2, FFmpeg 9, glibc 2.44, GCC 16), so the bundle
carries the real Breeze look (docs/archive/plan.md 4): the Breeze style plugin, KDE's icon engine, the Breeze
icons and the KDE colour schemes. Verified to run on the build container and, from a clean
environment, on Linux Mint 22 / Ubuntu 24.04 (glibc 2.39, Breeze 5 icons on the host) with
identical rendering.

Other formats ([Flatpak](https://flatpak.org), distribution packages, ...) may come later; for now the AppImage is
the only release artefact.

## Build

```sh
# in the container (repository mounted at the same path):
podman exec -w "$PWD" kk-arch env KK_BUILD_DIR=build-arch-release scripts/package_linux.sh  # Release build + AppDir + AppImage
podman exec -w "$PWD" kk-arch scripts/check_bundle.sh                                         # verify the AppDir and the AppImage
```

`KK_BUILD_DIR` may also point at an existing build outside the source tree (with
`--no-build`), e.g. a worktree packaging the main checkout's `build-arch-release`.

Options: `--no-build` (reuse `$KK_BUILD_DIR/kinoklip`), `--no-appimage` (stop after the
AppDir), `--run` / `--tarball` (also make the legacy self-extracting `.run` / the `.tar.zst`
archive of the AppDir; neither is released any more). Environment: `KK_BUILD_DIR`, `KK_JOBS`,
`KK_TOOLS_CACHE` (default `.cache/appimage-tools`, gitignored), `KK_UPDATE_INFO` (embedded
update information, `""` for none), `KK_ZSTD_LEVEL` (default 19), `KK_COMPRESSION`
(`--run`/`--tarball` only), `KK_QT_PLUGINS`. The script still works on a Debian/Ubuntu host
(Qt 6.4, no KF6: no Breeze parts, licences via dpkg) as a fallback.

Packages the container needs beyond the build dependencies: `patchelf file curl`
(`appstreamcli` validates the AppStream file when present; licences are collected with
`pacman -Qo/-Qi` and `/usr/share/licenses`). No FUSE is needed to build: appimagetool, itself
an AppImage, runs with `APPIMAGE_EXTRACT_AND_RUN=1`.

| Output | Size (0.4.0-alpha.1, container build) | What |
|---|---|---|
| `dist/Kinoklip-x86_64.AppImage` | 101 MB | **the release file** |
| `dist/Kinoklip-x86_64.AppImage.zsync` | 0.2 MB | delta-update metadata, uploaded next to it |
| `dist/kinoklip-x86_64/` | 330 MB | the AppDir (start `./AppRun` for quick tests) |

## Files in this directory

| File | Purpose |
|---|---|
| `excludelist.txt` | which libraries come from the host (`host`), are only a fallback (`fallback`) or a fallback that also replaces an older host copy (`newer`), with reasons |
| `apprun.sh` | the launcher, copied to `<bundle>/AppRun` |
| `runheader.sh` | shell header of the legacy `.run` file (`--run` only; template filled by `package_linux.sh`) |
| `org.kinoklip.Kinoklip.desktop` | desktop entry (validated with `desktop-file-validate`); `X-AppImage-Version` is appended in the AppDir |
| `org.kinoklip.Kinoklip.metainfo.xml` | AppStream metadata, installed as `usr/share/metainfo/org.kinoklip.Kinoklip.appdata.xml` with the version and date filled in (validated with `appstreamcli validate --no-net`) |
| `org.kinoklip.Kinoklip.xml` | MIME type `application/x-kinoklip-project` for `*.kkproj` |

## AppDir layout

```
kinoklip-x86_64/
  AppRun                              launcher (POSIX sh)
  org.kinoklip.Kinoklip.desktop       desktop entry + X-AppImage-Version  } read by appimagetool,
  org.kinoklip.Kinoklip.png, .DirIcon icon (256 px PNG; .DirIcon a copy)  } Gear Lever, appimaged
  usr/bin/kinoklip                    RUNPATH $ORIGIN/../lib:$ORIGIN/../lib/fallback-active
  usr/lib/*.so*                       FFmpeg + codecs (x264, x265, dav1d, aom, svt-av1, vpx,
                                      rav1e, opus, lame, vorbis, webp, jxl, ...) and their deps
  usr/lib/*.so*                       also KDE Frameworks (KColorScheme, KIconThemes, KConfig,
                                      KWidgetsAddons, KGuiAddons, KArchive, KI18n,
                                      libKF6BreezeIcons with the icons compiled in)
  usr/lib/qt/*.so*                    Qt 6.12 libraries and libraries only Qt needs (ICU, ...)
  usr/lib/qt/qt-version.txt           exact Qt version built against (host-Qt mode needs it)
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
  usr/lib/fallback/                   prefer-host libraries (zlib, glib, X11, xcb, freetype,
                                      the GL dispatchers, ... about 80 in all) and
                                      "newer" ones (libstdc++, libgcc_s, libva, fontconfig,
                                      wayland-client/-cursor) + newer.txt (their version tags)
  usr/lib/fallback-active/            symlinks to the fallbacks the host lacks (made by AppRun)
  usr/share/color-schemes/            KDE colour schemes (Kinoklip = default, BreezeDark, BreezeLight, BreezeClassic)
  usr/share/icons/breeze/             Breeze icon theme (wins over an older host Breeze)
  usr/share/kinoklip/fonts/           NotoColorEmoji.ttf (OFL 1.1) for emoji in titles and captions;
                                      used only when the host has no colour emoji font
  usr/share/applications, icons/hicolor/<n>x<n>/apps (icon PNGs, 16-512 px), mime/packages
  usr/share/metainfo/org.kinoklip.Kinoklip.appdata.xml   AppStream metadata
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
look host-dependent: the app sets style, colours and icons itself; the portal file dialogs
need no platform theme, see "File dialogs" below), `kstyle_config`, TLS/SQL/print
plugins. `qt.conf` is deliberately not used: Qt reads it from the binary's directory in
*every* mode, which would break the host-Qt mode; `QT_PLUGIN_PATH` does the same job.

## What comes from the host, and why

Full list with comments: `excludelist.txt`. The rule since GitHub issue 1 (Fedora names it
`libbz2.so.1`, so `libbz2.so.1.0` was nowhere to be found, and the app silently did not start):
**the bundle needs nothing from the host but glibc and a kernel.** Every other library it links
is in the bundle, and the host copy wins whenever the host has one, so nothing changes on a system
that has the library:

- **glibc** (libc, libm, ld-linux, ...): the host's, when it is new enough. An Arch build
  references the newest libm symbol versions (`sqrtf@GLIBC_2.43`, `cosh@GLIBC_2.44`, from
  FFmpeg, Qt, codecs), so on older hosts (Ubuntu 24.04 / Mint 22: 2.39) AppRun starts the
  binary through the bundled loader instead: `usr/lib/glibc/ld-linux-x86-64.so.2
  --library-path usr/lib/glibc:... usr/bin/kinoklip` (only when `getconf GNU_LIBC_VERSION`
  is older than `usr/lib/glibc/glibc-required.txt`). glibc is backwards compatible, so host
  GPU drivers and other host libraries load fine; ld.so options are not inherited, so child
  processes use the host glibc. `/proc/self/exe` is then the loader (the app does not use
  `applicationDirPath()`; `KINOKLIP_APPDIR` is set). Locale data comes from the host.
- **GPU drivers** (`libGLX_*`, `libEGL_*`, gbm, `libdrm_*`, CUDA/NVIDIA): the only other `host`
  entries; nothing in the bundle links them, they are found at run time. They must match the
  installed kernel module and hardware. A bundled driver could never be right.
- **libstdc++/libgcc_s**: "newer" fallbacks: GCC 16's runtime (GLIBCXX_3.4.36) is used when
  the host's is older (Mint 22: 3.4.33); a host with the same or newer GCC runtime (KDE Linux)
  keeps its own, so host Mesa/LLVM never get an older one than they were built for.
- **GPU front ends** (the glvnd dispatchers libGL/EGL/GLX/OpenGL/GLdispatch, libvulkan, libdrm,
  libvdpau, libOpenCL): fallbacks. They contain no driver: they look for the host's drivers. On a
  host without them the app still starts and draws in software. **libva** is a
  "newer" fallback (Arch FFmpeg calls `vaMapBuffer2`, libva >= 2.21); when the bundled libva
  is used, AppRun sets `LIBVA_DRIVERS_PATH` to the host's driver dirs (unless the user set
  it), otherwise `LIBVA_DRIVERS_PATH` is never touched.
- **Wayland client libs, xkbcommon, X11/xcb core**: fallbacks; the host's follow the compositor
  and keymaps. libwayland-client/-cursor are "newer" fallbacks (KF6GuiAddons uses
  `wl_display_create_queue_with_name`, wayland >= 1.23; the protocol stays compatible), and
  libxkbcommon and libxkbcommon-x11 too (with Fedora 43's 1.11.0 the bundled Qt crashed on exit
  under X11). Measured: a GNOME install on Ubuntu 24.04 and 26.04 lacks `libxcb-shape`, `libxcb-xinput`
  (26.04 also `libxcb-xkb`), which Qt's xcb platform plugin links, so `--version` worked while the
  window did not open when the app had to use X11.
- **fontconfig/freetype/harfbuzz/graphite2**: fallbacks, so the host fonts and configuration
  are found; the host freetype links harfbuzz and graphite2. fontconfig is a "newer"
  fallback (pango/libass from Arch call `FcConfigSetDefaultSubstitute`, fontconfig 2.17); it
  reads the host's /etc/fonts.
- **Audio** (libpulse, with libsndfile, libasyncns and libFLAC, its dependencies), **dbus,
  systemd/udev, glib/gio**: fallbacks. Distributions without systemd have no libsystemd, which Arch's
  Qt links.
- **zlib, zstd, xz, bzip2, lz4, brotli, expat, libffi, libpng**: fallbacks: shared with Mesa/LLVM/
  freetype/systemd. A bundled older copy would be loaded first and shadow the newer one those
  host libraries were built against, so the host's wins.

A library the bundle carries only because a fallback needs it must be a fallback itself:
an ordinary bundled one is on `LD_LIBRARY_PATH` and would shadow the copy host libraries want.

`scripts/check_bundle.sh` proves that every direct dependency of every bundled ELF resolves
inside the bundle or to one of these, once normally and once with all fallbacks forced, and
prints the minimum host glibc without the bundled one plus the libraries the host may provide.
It also fails on a `host` entry without a stated reason (`host <soname> # why`), a host-only soname
known to differ between distributions, a bundled file needing anything host-only but glibc, and an
ordinary bundled library that only a fallback needs. Symbol-level gaps against an older host (what decided the "newer" entries above) are
found on that host with the bundled loader:

```sh
L=dist/kinoklip-x86_64/usr/lib
for f in dist/kinoklip-x86_64/usr/bin/kinoklip $(find $L/qt/plugins $L/libheif -name '*.so'); do
    env -i LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes \
        $L/glibc/ld-linux-x86-64.so.2 --library-path $L/glibc:$L/qt:$L/fallback-active "$f"
done | grep -E 'undefined symbol|not found'
```

## The launcher (`AppRun`)

1. Links the fallback libraries the host lacks into `usr/lib/fallback-active/`, plus the "newer" ones whose host copy
   is older (highest `GLIBCXX_`/`GCC_` tag in the file, or the version in the real file name,
   against `usr/lib/fallback/newer.txt`). RUNPATH is searched before the system paths, so
   only those may be linked there. In a read-only bundle -- the AppImage's squashfs mount --
   the links go into a private writable directory instead
   (`$XDG_RUNTIME_DIR/kinoklip-fallback-<hash>/active`, else `~/.cache/kinoklip/...`, else a new
   private `mktemp` directory; a noexec mount is fine, these are only links, and with nowhere
   writable left all fallbacks are used, the host copies losing out for that start),
   which is put first in `LD_LIBRARY_PATH` (recorded like every other variable,
   so child processes do not inherit it). That directory also links `usr/lib/*.so*` and
   `usr/lib/qt`, so the fallbacks' own RUNPATH (`$ORIGIN/..`) resolves as in the bundle.
   An AppImage is mounted at a new path on every start; directories of mounts that are gone
   are removed on the next start.

   "The host has it" means that the loader which will run the app will find it: an x86-64 entry of
   the `ldconfig -p` cache whose file exists, else a 64-bit file in that loader's own default
   directories. Those differ: Fedora/openSUSE `/lib64`, `/usr/lib64`; Debian/Ubuntu the multiarch
   directories and `/lib`, `/usr/lib`; Arch `/usr/lib`; **the bundled Arch ld.so only `/usr/lib`**,
   so with the bundled glibc Fedora's `/usr/lib64` is reached through the cache alone. A library
   there that is missing from the cache (no `ldconfig` run yet) counts as absent in that mode. On
   Fedora `/usr/lib` holds 32-bit libraries (Steam pulls in many), which never count.
2. Chooses the Qt:
   - **Default: bundled Qt 6.12 + KF6 + Breeze.** `LD_LIBRARY_PATH=usr/lib/qt`,
     `QT_PLUGIN_PATH=usr/lib/qt/plugins` (host plugins are built for another Qt/KF6),
     `QT_QPA_PLATFORMTHEME=generic` and `QT_STYLE_OVERRIDE` unset, so neither a host platform
     theme nor Qt's built-in KDE theme can restyle the app: Breeze style, KDE colour scheme and
     Breeze icons come from the bundle and look identical on every system (the user changes
     them in Settings > Color Theme / Style). `QT_MEDIA_BACKEND=ffmpeg` (unless set); `QT_AUDIO_BACKEND=pulseaudio` (unless set) when no PipeWire client.conf exists on the host (Qt 6.12's PipeWire backend crashes then).
   - **Opt-in: `KINOKLIP_HOST_QT=1`** uses the system Qt only if it is **exactly** the Qt
     release in `usr/lib/qt/qt-version.txt` (major.minor.patch, e.g. 6.12.0), has every module
     in `host-qt-requirements.txt` from that same release, and has a wayland or xcb platform
     plugin; otherwise it falls back to the bundled Qt and logs why (`host Qt 6.12.1 is not the
     Qt 6.12.0 the app was built with`). The exact match is required because the app links Qt
     GuiPrivate (the Wayland parent-window handle for the portal file dialog,
     src/platform/PortalFileChooser.cpp): private Qt ABI changes with every release, patch
     releases included, and a mismatch can crash at any point. Bundles that recorded only
     "6.12" (alpha.3 and older) never use the host Qt. In practice host-Qt mode only helps on
     a distribution that ships the very Qt the bundle was built with (Arch, right after a
     release); it exists for debugging Qt-side problems against a host Qt build. Nothing Qt-related is set, so the host
     Qt finds its own plugins (incl. the host's Breeze). FFmpeg, codecs and the KF6 libraries
     still come from the bundle.
   - `KINOKLIP_BUNDLED_QT=1` forces the bundled Qt (wins over `KINOKLIP_HOST_QT`).
   The choice is logged to stderr (`KINOKLIP_QUIET=1` silences it).
3. `XDG_DATA_DIRS=usr/share:<previous or /usr/local/share:/usr/share>` (KColorSchemeManager
   finds the colour schemes, KIconLoader the Breeze icons there),
   `LIBHEIF_PLUGIN_PATH=usr/lib/libheif/plugins`, `QT_QPA_PLATFORM=wayland;xcb` when
   `WAYLAND_DISPLAY` is set and `QT_QPA_PLATFORM=xcb` when it is not (a pure X11 session: no
   "Failed to create wl_display" / "Could not load the Qt platform plugin wayland" lines
   before the fallback), unless the user set `QT_QPA_PLATFORM`; exports `KINOKLIP_APPDIR`.
4. Every variable it changes is recorded first: `KINOKLIP_ORIG_VARS` lists the names and
   `KINOKLIP_ORIG_<NAME>` holds the original value (absent = was unset). The app
   (`src/platform/ChildProcessEnv.h`: `childEnvironment()`, `prepareChildProcess()`,
   `openUrl()`) restores exactly that for programs it starts (xdg-open, the file manager,
   ...), so they never see the bundle's Qt, plugins or data dirs.
5. Execs `usr/bin/kinoklip` with all arguments -- through the bundled glibc loader if the
   host glibc is too old (see above; `KINOKLIP_FORCE_GLIBC=1|0` test hook).

**A failed start is never silent.** The launcher execs, so that the app is this process (signals,
exit code, process id), which leaves nothing behind to report a loader failure. So it asks the loader
first, in its trace mode (what `ldd` uses), which runs none of the app's code:

- every start: the binary, about 15 ms: missing libraries and symbol versions;
- once per state of the bundle and the host's libraries (time and size of `usr/bin/kinoklip` and
  `/etc/ld.so.cache`, the fallback list, `LD_LIBRARY_PATH`; kept in `~/.cache/kinoklip/loader-check`)
  also every symbol, plus the Qt platform plugins, which only load at run time: about 200 ms.

If the loader reports nothing, which is the normal case, the app is exec'ed as always. Fallbacks can
be a mixed set: a bundled library the host lacks can need a newer library than the host's copy of
another (Ubuntu 24.04 without a desktop: the bundled libgio wants a newer libmount). So the launcher first
repairs what the loader reports: `version ... not found` for a host library with a bundled copy
activates that copy, and if problems remain, all fallbacks are used together, since they fit each other.
The host copy stays everywhere the loader has nothing to say. A report that remains never
stops the app, because lazily bound symbols may never be used: the launcher keeps running as the app's
parent instead (passes on TERM, HUP and INT, exit status, stdin; stderr through `tee`). If the app
then exits with 126 or 127, says it has no Qt platform plugin, or aborts or crashes within 3 seconds,
the launcher shows the loader's line and a link to the issue tracker: on stderr, in
`~/.cache/kinoklip/start-failure.log`, and, unless started from a terminal, through `kdialog` /
`zenity` (the desktop's own first), `notify-send`, `gdbus` to the notification service or `xmessage`,
with the user's original environment. Nothing is sent anywhere (docs/privacy.md). A start that works
is remembered, so the next one execs again. `KINOKLIP_NO_PREFLIGHT=1` skips the check,
`KINOKLIP_NO_FAILURE_DIALOG=1` keeps it to stderr.

Not caught: a failure after the exec, once a state passed the once-per-state check (nothing
changed in between, so it should not happen), and whatever happens before AppRun runs, in the
AppImage runtime: FUSE missing or a read-only `TMPDIR` for its mount point only gives a message on
stderr. [Gear Lever](https://flathub.org/apps/it.mijorus.gearlever) users would see nothing in that case; see "Known risks".

`AppRun --install-desktop-entry` writes the desktop entry, icon and MIME type to
`${XDG_DATA_HOME:-~/.local/share}` with `Exec=` pointing at the AppImage file (`$APPIMAGE`, set
by the AppImage runtime), the legacy `.run` file or AppRun for a plain folder, then refreshes
the MIME/desktop/KDE caches if the tools exist. `--uninstall-desktop-entry` removes them.

**Not automatic in the AppImage.** When `$APPIMAGE` is set the launcher leaves desktop
integration to Gear Lever (or [appimaged](https://github.com/probonopd/go-appimage) / [AppImageLauncher](https://github.com/TheAssassin/AppImageLauncher)): they write their own entry with
`Exec=` the AppImage, and a second entry of ours would show the app twice in the menu. Users
without such a tool can run `./Kinoklip-x86_64.AppImage --install-desktop-entry` once.

For the plain AppDir and the legacy `.run`, every normal start also does this automatically (quietly) when
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

## The AppImage

`package_linux.sh` runs **appimagetool 1.9.1** with the **type 2 static runtime 20251108**
(github.com/AppImage/appimagetool, github.com/AppImage/type2-runtime), both downloaded into
`$KK_TOOLS_CACHE` and checked against the SHA-256 pinned in the script (update both together:
`gh api repos/AppImage/appimagetool/releases` lists the digests). The squashfs uses zstd level
19 with 1 MiB blocks (101 MB instead of 110 MB at the defaults) and file times from the last
commit (`SOURCE_DATE_EPOCH`). appimagetool's own AppStream check is switched off
(`--no-appstream`) because it fetches every URL in the file and fails while the GitHub
repository is private; the script validates it offline instead.

**Update information** (embedded, read by Gear Lever and [AppImageUpdate](https://github.com/AppImageCommunity/AppImageUpdate)):

```
gh-releases-zsync|SeniorMatt|kinoklip|latest-all|Kinoklip-*x86_64.AppImage.zsync
```

`latest-all`, not `latest`: `latest` means GitHub's `/releases/latest`, which never returns a
pre-release, and every Kinoklip release is a pre-release (alpha now, beta later). With `latest`
no update would ever be found. `latest-all` takes the newest release, pre-release or not, so
it keeps working after 1.0 too. Gear Lever 4.6 (`GithubUpdater`) and AppImageUpdate
(`GithubReleasesZsyncUpdateInformation`) both support it; checked with Gear Lever on a public
repository that has only pre-releases: `latest` finds nothing, `latest-all` reports the
update. Gear Lever compares the SHA-1 in the release's `.zsync` with the installed file, so
**every release must upload both `Kinoklip-x86_64.AppImage` and
`Kinoklip-x86_64.AppImage.zsync`** (file names without the version, so the pattern and the
`URL:` line of the `.zsync` stay valid). While the repository is private the GitHub API answers
404 to anonymous requests and update checks find nothing.

**FUSE.** The static runtime needs no libfuse on the host, only the kernel FUSE module and
`fusermount`/`fusermount3` (present on KDE Linux, Arch, Ubuntu, Fedora ...). Checked in the runtime
20251108 binary (strings): it has **no automatic fallback**. `APPIMAGE_EXTRACT_AND_RUN` and
`--appimage-extract-and-run` are only read when the user sets them; without a usable `fusermount` it prints
"Cannot mount AppImage, please check your FUSE setup" to stderr and exits, so a double-click shows nothing.
The runtime cannot be changed (no custom runtime), so the release notes and the download block in
docs/releases/readme.md tell users in advance. Without FUSE:
`./Kinoklip-x86_64.AppImage --appimage-extract-and-run` (or `APPIMAGE_EXTRACT_AND_RUN=1`)
unpacks into a temporary directory and runs from there; `--appimage-extract` unpacks to
`squashfs-root/` permanently. Other runtime flags: `--appimage-help`,
`--appimage-updateinformation`, `--appimage-version`.

**Gear Lever** (Flatpak `it.mijorus.gearlever`, the owner's AppImage manager) reads the name,
the version (`X-AppImage-Version`) and the icon (`.DirIcon`) from the image, **moves** it to
`~/AppImages/kinoklip.appimage`, writes `~/.local/share/applications/kinoklip.desktop` with
`Exec=` that file and the icon in `~/AppImages/.icons/`, and lists the GitHub update source as
"GithubUpdater | embedded". Its 7-Zip extractor prints "Dangerous link path was ignored" for the
relative symlinks of the bundled Breeze icon theme; harmless, it only needs the top-level files.
CLI check (in a throwaway `HOME`; `FLATPAK_USER_DIR` keeps the user's Flatpak installation):

```sh
G=$(mktemp -d); R=$(mktemp -d /tmp/glr.XXXX)    # short path: it holds Flatpak's sockets
gl() { env -i HOME=$G PATH=/usr/bin:/bin FLATPAK_USER_DIR=$HOME/.local/share/flatpak \
    XDG_RUNTIME_DIR=$R dbus-run-session -- flatpak run --user it.mijorus.gearlever "$@"; }
gl --integrate dist/Kinoklip-x86_64.AppImage -y
gl --list-installed -v     # Kinoklip (0.4.0-alpha.1)  [0.4.0-alpha.1]  [GithubUpdater|embedded]
gl --list-updates -v
gl --remove $G/AppImages/kinoklip.appimage -y; rm -rf $G $R
```

## The legacy single-file `.run` (`--run`, not released)

Released up to 0.4.0; replaced by the AppImage. Kept behind `--run` while it costs nothing.
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

## File dialogs (XDG desktop portal)

The app shows the desktop's own file chooser (on Plasma the KDE / Dolphin dialog) by calling
`org.freedesktop.portal.FileChooser` over D-Bus itself (`src/platform/PortalFileChooser`,
details in `src/platform/readme.md`). Nothing extra is bundled for it: only `libQt6DBus`
(already part of the bundled Qt) is needed, the dialog itself runs in the host's
`xdg-desktop-portal` + `xdg-desktop-portal-kde` processes, and the D-Bus daemon is the host's.

Deliberately **not** used: Qt's `xdgdesktopportal` platform theme plugin
(`QT_QPA_PLATFORMTHEME=xdgdesktopportal`). It would also give portal dialogs, but it wraps
the platform theme chosen from `XDG_CURRENT_DESKTOP` (Qt's built-in KDE theme on Plasma), which
imports the host's fonts, palette and icon settings: the look would become host-dependent and
`src/app/Theme`'s "system" palette capture would change. The KDE platform theme
(plasma-integration) has the same problem and is built against a specific KF6/Qt. With the
direct D-Bus implementation the launcher keeps `QT_QPA_PLATFORMTHEME=generic`.

Fallback: no session bus, no portal (`org.freedesktop.portal.Desktop` neither running nor
activatable), no file chooser backend, or an error -> Qt's widget dialog, as before.
`KINOKLIP_FILE_DIALOG=qt` forces it. A warning `kinoklip.filedialogs: portal file dialog
failed` is logged when the portal was found but failed; `QT_LOGGING_RULES=kinoklip.filedialogs.debug=true`
shows the probed version.

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
    dist/Kinoklip-x86_64.AppImage --color-scheme BreezeLight --screenshot preferences /tmp/prefs.png
# the real window under Xvfb, from the FUSE mount and without FUSE
xvfb-run -a -s '-screen 0 1920x1080x24' dist/Kinoklip-x86_64.AppImage
xvfb-run -a -s '-screen 0 1920x1080x24' dist/Kinoklip-x86_64.AppImage --appimage-extract-and-run
```
AppImage, 0.4.0-alpha.1, on the Ubuntu 24.04-based host (glibc 2.39) in a clean environment
(`env -i`, empty `HOME`/XDG dirs): from the FUSE mount the launcher reports `using bundled
fallbacks: libfontconfig.so.1 libstdc++.so.6 libva-drm.so.2 libva-x11.so.2 libva.so.2
libwayland-client.so.0 libwayland-cursor.so.0 (links in $XDG_RUNTIME_DIR/kinoklip-fallback-<n>/active)`
and the bundled glibc; the window shows with the Breeze look, and no desktop entry is written.
`--appimage-extract-and-run` and the Gear Lever menu entry give the same result.

On Mint the launcher reports `using bundled fallbacks: libfontconfig.so.1 libstdc++.so.6
libva-drm.so.2 libva-x11.so.2 libva.so.2 libwayland-client.so.0 libwayland-cursor.so.0` and
`using the bundled glibc (host: 2.39, bundle needs: 2.44)`; the screenshots are pixel-identical
in style and icons to the container's, and CUDA/Vulkan decoding is detected with the host's
NVIDIA driver.

With `LD_DEBUG=libs` the loaded objects were confirmed: bundled mode loads Qt and plugins
only from the bundle; `KINOKLIP_HOST_QT=1` loads `/lib/x86_64-linux-gnu/libQt6Core.so.6` and
the host's platform plugin; with `KINOKLIP_HOST_LIBDIRS=<empty dir>` the host Qt is rejected
and libva/libvdpau come from `fallback-active/`.

File dialogs through the portal (needs `xdg-desktop-portal`, `xdg-desktop-portal-kde`,
`xorg-server-xvfb`, `imagemagick`, `xdotool` in the container):

```sh
podman exec -w "$PWD" kk-arch scripts/portal_dialog_check.sh dist/kinoklip-x86_64/AppRun /tmp/pc
# also choose two files in the KDE dialog and check they are imported:
podman exec -w "$PWD" kk-arch env KK_PORTAL_CHOOSE='"/tmp/m/a.mp4" "/tmp/m/b.mp4"' \
    KK_SCREENSHOT_DIALOG_DELAY=14000 KK_ROOT_GRAB_AFTER=4 scripts/portal_dialog_check.sh \
    dist/kinoklip-x86_64/AppRun /tmp/pc
```
Done for this change: the KDE (KIO) dialog appears over the bundled app's window with the
"Media files" filter selected, `dbus.log` shows `FileChooser.OpenFile` with `multiple=true`,
`modal=true`, the `x11:` parent, case-insensitive globs and the remembered `current_folder`,
the `Response` URIs (one with a space) are imported, and quitting with the dialog open sends
`Request.Close`. Under headless weston (Wayland) the call is made too; weston has no
xdg-foreign, so it goes without a parent there.

### Checking other distributions

```sh
scripts/check_distros.sh                        # outside the container, needs podman; one to two hours
scripts/check_distros.sh --only fedora:44       # one image
```
It starts the AppImage in official images of Fedora, Ubuntu, Debian, openSUSE Tumbleweed and Arch,
with a GNOME or KDE library set (no weak dependencies) or the bare image: `--version` from the
read-only bundle and through the AppImage file, the same with no writable place or a noexec one for the
launcher's links, and the edit page screenshot under Xvfb (X11) and on a headless weston (Wayland), then
prints a pass or fail table with the loader's error. It removes the images it pulled.
It cannot test real GPUs, real desktop sessions, FUSE or SELinux.

### Release steps for Linux

1. `podman exec -w "$PWD" kk-arch env KK_BUILD_DIR=build-arch-release scripts/package_linux.sh`
2. `podman exec -w "$PWD" kk-arch scripts/check_bundle.sh`
3. `scripts/check_distros.sh`: every row must pass. When an image's distribution has a new release,
   add it to the list at the top of the script.
4. `scripts/publish_release.sh <tag> "<title>"`, after the owner's go.

## Known risks on the target

- The AppImage runtime starts before the launcher. Without FUSE (`fusermount3` or `fusermount`
  missing: measured on a no-recommends Ubuntu 26.04 GNOME set) or without a writable `TMPDIR` for its
  mount point, it only prints to stderr, so a start from a menu shows nothing. `--appimage-extract-and-run`
  or `APPIMAGE_EXTRACT_AND_RUN=1` works around it (no automatic fallback exists in the runtime; verified from its strings). Fixing it would mean changing the upstream runtime, so the release notes say what to do: install `fuse3` (Debian/Ubuntu: `sudo apt install fuse3`; Fedora: `fuse`; Arch: `fuse3`), or run from a terminal with `--appimage-extract-and-run`.
- SELinux could not be tested in a container. By the code: the links and files the launcher makes are
  in the user's own directories, and the app's libraries are mapped from the FUSE mount (type
  `fusefs_t`) as for every AppImage. On a default Fedora targeted policy a user session is unconfined.
  No bundled ELF has text relocations or an executable stack (checked with `readelf`), so the
  execmod/execstack denials do not apply. Not verified on an enforcing system.
- A downloaded AppImage is not executable: Dolphin offers to make it executable on
  double-click (or Properties > Permissions > "Is executable", `chmod +x`); opening it with
  Gear Lever avoids that and adds it to the menu.
- Under Gear Lever the menu entry is `kinoklip.desktop`, while the app's Wayland app id is
  `org.kinoklip.Kinoklip`. Plasma may then not match the running window to the entry (a
  generic task-manager icon on Wayland) and Qt may log "Failed to register with host portal".
  Not yet checked on a real Plasma session; if it shows, the fix is a hidden
  (`NoDisplay=true`) `org.kinoklip.Kinoklip.desktop` written by AppRun.
- Users who ran an older `.run`/folder have `~/.local/share/applications/org.kinoklip.Kinoklip.desktop`
  pointing at it; with Gear Lever the menu then shows Kinoklip twice. Remove the old one with
  `<old bundle>/AppRun --uninstall-desktop-entry` (or delete the file).
- Programs the app starts get the original environment back only where the app goes through
  `src/platform/ChildProcessEnv` (`QProcess`) or `platform::openUrl` (links, files, folders:
  the OpenURI portal first, src/platform/readme.md "Opening links"). Never call
  `QDesktopServices::openUrl` directly: on Wayland Qt starts the helper later, with the
  bundle's environment, and not at all when the calling dialog has closed (1.1.4-beta.1).
- File dialogs come from the XDG desktop portal (the KDE/KIO dialog on Plasma) when the
  session has one; otherwise Qt's widget dialog. See "File dialogs" above. Not yet checked on
  a real Plasma 6 Wayland session: whether KWin stacks the portal dialog over our window
  (it is exported through xdg-foreign) and that keyboard focus moves to it.
- The Wayland app_id is `org.kinoklip.Kinoklip` (`setDesktopFileName`), matching the desktop
  file name; on X11 KWin matches through the same desktop file name, and
  `StartupWMClass=kinoklip` still matches the WM_CLASS instance (binary name).
- Bundled-glibc mode (older hosts only): NSS modules and gconv come from the host's glibc
  version and may not load into the newer glibc (user names via NSS, iconv charsets); Qt uses
  neither for normal operation. KDE Linux / current Arch never use this mode.
- Host libraries newer than ours with the same soname are shared (e.g. bundled `libwebp`,
  `libjpeg` vs. the host's in host-Qt mode). Not seen in tests; watch for "undefined symbol"
  warnings from host image plugins in host-Qt mode.
