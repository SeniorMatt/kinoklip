# src/app — application shell

Owns the process-level wiring: `main.cpp` creates the model objects and the `AppContext`,
`MainWindow` arranges the pages, and a few controllers implement app-level commands.
Nothing in `src/ui/**` or below may include from here (dependency direction PLAN §5 / CODING_STYLE §4).

## Key classes

| File | What |
|---|---|
| `AppInfo.h` | product name/id/version (from CMake) — rename the app there + `CMakeLists.txt` |
| `MainWindow.h` | menu bar (built from `ActionRegistry`), `InterfaceBar`, page stack, `PageBar`; window/page layout persistence; close prompt |
| `InterfaceBar.h` | top strip: panel toggles left/right, project name centred |
| `PageBar.h` | bottom strip in Resolve's order: Media · Cut · Edit · Compositing · Color · Audio · Deliver (unbuilt pages disabled; slots are the `kPageSlots` table in `MainWindow.cpp`) |
| `pages/AudioPage.h` | the Audio page (spec 37): small floatable `ViewerPanel`, `LoudnessPanel` (meters, loudness), a `TimelineWidget` with the audio page view options (tall audio, collapsed video, headers with arm / format / meter) driven by the Edit page's `TimelineController`, the large `MixerPanel`; own `MixerController` (Write passes while shown) and `MeterModel`; widgets in `src/ui/audio`. `ScreenshotAudio.h` builds the `audio[:eq|dynamics]` screenshot (demo mix, offline meter reading) |
| `pages/CompositingPage.h` | the Compositing page (spec 42b): two `CompViewer`s, `CompTransport`, the node tool bar and `NodeEditor`, `CompInspector`, `NodeLibraryPanel`; one `CompSession` holds the open clip's composition; connects the comp.* commands (widgets in `src/ui/comp`) |
| `pages/CutPage.h` | the Cut page (spec 35): media pool, `CutViewer` (Source Clip / Source Tape / Timeline), `CutToolBar`, dual timeline (`CutOverview` over a centred-playhead `TimelineWidget`); edits with the Edit page's `TimelineController` (built after the Edit page in `MainWindow`); widgets in `src/ui/cut` |
| `pages/EditPage.h` | PLAN §4 Edit layout with nested `QSplitter`s; accessors for every panel |
| `pages/MediaPage.h` | browser · source viewer · metadata over the media pool |
| `ProjectController.h` | new / open / save / save as / import media + "save changes?"; single-file projects (`ProjectPackaging.cpp`: the open `pack::ProjectPackage` + `ProjectLock` for the project's lifetime, read-only when locked elsewhere or not writable, Save As between the kinds = Pack / Unpack, File > Pack / Unpack / Compact / Verify Project, the AppContext's `MediaStore` that copies imports in on a worker thread); window title "(Read-Only)"; File > Open Recent; start-up crash recovery prompt; the Project Manager (`showProjectManager()`: owns the `ProjectLibrary` at `prefs::projectLibraryDirectory()`, the window from `ui/projects`, opens what it asks for after `maybeSave()`), `openLastProject()` |
| `TimelineExport.h` | File > Export: Timeline as EDL, Timeline Markers to EDL / CSV (save dialog + `export/EdlExport.h`) |
| `AutosaveController.h` | autosave every `prefs::autosaveMinutes()` (2) to `AppDataLocation/autosave/`; the file is deleted when saved / replaced / on normal quit, so one left at start-up means a crash (`findRecoverable()`); the timer writes in the background (`Project::captureSave()` on the GUI thread, JSON + file on a worker) |
| `Theme.h` | the real KDE Breeze look: widget style, KDE colour schemes, Breeze icons, panel style sheet; Workspace > Color Theme / Style; "appearance" settings page |
| `ColorSchemeFile.h` | reads a KDE `.colors` file into a QPalette (fallback build without KF6) |
| `CommandLine.h` | `QCommandLineParser` → `CommandLineOptions` |
| `Screenshot.h` | `--screenshot <target> <out.png> [project]`: pages, `preferences[:section]`, `project-settings[:section]`, `keyboard`, `inspector`, `mediapool`, `menu-color-theme`, `menu-style`, `cut[:tape|clip|strip]` (an empty timeline first gets the media pool's files appended), `audio[:eq|dynamics]` (Audio page with a demo mix and meter readings; the editor window composited in), `new-project` (the New Project dialog), `project-manager` (the Project Manager over the preferences' library or `KK_PROJECT_LIBRARY`; a project argument is marked open) (works with `QT_QPA_PLATFORM=offscreen`; `KK_SCREENSHOT_SIZE=1366x768` for small screens; `KK_SCREENSHOT_PLAY_MS=2000` plays the first viewer with content that long before the grab, a headless playback check) |
| `MenuMetricsStyle.h` | proxy over the widget style that reserves the submenu arrow column in menus |

## Start-up sequence (`src/main.cpp`)
1. `configureOpenGL()` — default `QSurfaceFormat` = OpenGL 3.3 core, `Qt::AA_ShareOpenGLContexts`
   (must happen before `QApplication`).
2. `theme::initBeforeApplication()` (KIconTheme: bundled Breeze icons), `QApplication`,
   desktop file name `org.kinoklip.Kinoklip` (Wayland app_id), organisation/app names (QSettings → `~/.config/cutwise/Kinoklip.conf`),
   `kk::initResources()`, command line, `theme::setSessionOverrides()` + `theme::apply()`.
3. `Project`, `SelectionModel`, `ActionRegistry` (+ `loadDefaultKeymap()` +
   `registerStandardActions()` + `theme::registerActions()`), `SettingsPageRegistry`
   (standard pages + `theme::registerSettingsPage()`), `AppContext`.
4. Non-interactive modes (screenshot) or `MainWindow::showRestored()`, then
   `ProjectController::offerRecovery()` (autosaves of a crashed session), then the project
   argument; without one the Project Manager (`showProjectManager()`, non-blocking), or the
   last project when Preferences say "Reopen the last project" (`project/startupAction`).
   Offscreen runs never show the Project Manager.

## Settings keys used here
`mainWindow/geometry`, `mainWindow/state`, `mainWindow/page`, `pages/<pageId>/...` (splitter
states and `visible/<panel>` flags), `paths/lastProjectDir`, `paths/lastMediaDir`, `paths/recentProjects` (File > Open Recent, 10 entries). The Project Manager: `projectManager/*` (geometry, view, card size, sort, last New Project kind), preferences `project/libraryDirectory` and `project/startupAction` (`core/Preferences.h`). User
preferences (theme, hw acceleration, autosave) are in `core/Preferences.h`.

## Theme system (the real KDE Breeze, the Kdenlive way)

Same approach as Kdenlive on Windows/macOS: the app ships the actual Breeze Qt style plugin,
the Breeze icons and the KDE colour schemes, so it looks identical on every system, and the
user can change all of it.

| Part | KF6 build (Arch container, release bundle) | Fallback build (Qt 6.4, no KF6) |
|---|---|---|
| Widget style | `QApplication::setStyle("Breeze")` (plugin `styles/breeze6.so`); Fusion if missing | Breeze if the host has a Qt 6.4 build of it, else Fusion |
| Colours | `KColorSchemeManager::activateSchemeId()`; schemes from `color-schemes/` in `XDG_DATA_DIRS` (bundle: `usr/share/color-schemes`) and the app's `:/org.kde.kcolorscheme/color-schemes` (Kinoklip); a compiled-in scheme the model lacks (older KF6) is copied to the cache dir and applied directly | `ColorSchemeFile` parses the same `.colors` files (copies in `resources/color-schemes/` + XDG dirs) into a QPalette |
| Icons | `KIconTheme::initTheme()` before `QApplication`: Breeze compiled into libKF6BreezeIcons, drawn by KIconEngine which recolours them from the palette | `QIcon::setThemeName("breeze"/"breeze-dark")` by palette lightness if installed, else our SVGs |
| Chrome | style sheet generated from the palette (`Theme.cpp: styleSheet()`): only backgrounds/borders of our own panels and bars; buttons, tabs, menus, focus and selection are left to Breeze | same |

Defaults: style `Breeze`, scheme `Kinoklip` (`kDefaultColorScheme`, compiled into the app, so it
exists in every build mode); if it cannot be found, `BreezeDark`, then the style's palette. A
scheme the user chose explicitly (stored `ui/colorScheme`) is kept. "Default (System)" means the scheme in the
user's `kdeglobals` (Plasma's choice; KDE's default Breeze without one): palette and Header
colours both come from that file, never the palette from the platform and the header from
kdeglobals. Runtime switches are covered by `tests/app/test_themeswitch.cpp` (every scheme and
style in the real main window; every widget's palette and the painted chrome are checked).
Custom-drawn widgets take every colour from `kk::schemeColors()` (`ui/common/SchemeColors.h`,
see "Editor visuals") at paint time; icons drawn by
us use `kk::paletteIcon()` / the tinted SVG engine, which look the colour up per paint. Settings keys: `ui/widgetStyle`,
`ui/colorScheme` (`""` = "Default (System)"; the old `ui/useSystemColors=true` is migrated to
it). Command line (this run only, nothing stored): `--widget-style Fusion`,
`--color-scheme BreezeLight|system`.

**User interface.** Workspace > Color Theme and Workspace > Style are *data*: StandardActions
declares two submenu markers (`>settings.color_scheme`, `>settings.style`) in the `settings`
menu, and `theme::registerActions()` registers one checkable action per scheme/style in those
categories (ids `settings.color_scheme.<id|system>`, `settings.style.<key>`), so they are
also bindable in Keyboard Customization. Preferences shows the registered "appearance"
settings page (colour theme + widget style combos), which applies by triggering those same
actions, so menu, page and stored value never disagree.

**Icons.** `kk::icon(name)` (`ui/common/Icons.cpp`) maps our names to freedesktop/Breeze
names (`play` -> `media-playback-start`, `split` -> `edit-cut`, ...). Editor glyphs with no
Breeze counterpart (blade, trim, insert/overwrite, mark in/out, edit points, snapping, page
and some panel icons) stay our own SVGs, tinted from the palette like KIconEngine tints
Breeze. `tests/app/test_theme.cpp` fails if a mapped name disappears from Breeze.

**Editor visuals.** Everything Kinoklip paints itself follows the active scheme through
`kk::schemeColors()` (`ui/common/SchemeColors.h`): `theme::apply()` / `setColorScheme()` hand it
the KDE roles QPalette lacks (`SchemeRoles`: KColorScheme with KF6, the `.colors` file
without), it derives the editor roles (video clips = link, audio = positive, titles /
generators / subtitles = visited, selection = neutral, transitions = focus, playhead and marks
= negative, lanes / ruler / grids = view and window backgrounds, plates over video = the
complementary set) and emits `SchemeColorsNotifier::changed()`; widgets that cache colours or
pixmaps (`TimelinePainter::setStyle()`, effects library glyphs, viewer labels) refresh on it.
The viewer surround comes from the complementary set, so it stays dark in light schemes.
Fixed on purpose: clip colour labels, flags and marker colours (user data), video content
(letterbox black, thumbnails) and the white-on-black mask outlines over video.
`tests/app/test_schemecolors.cpp` switches schemes and checks the helper and the painted
timeline.

**Add a colour scheme:** drop a KDE `.colors` file into `~/.local/share/color-schemes/`
(or any `color-schemes/` dir in `XDG_DATA_DIRS`); it appears in Workspace > Color Theme on the
next start. To ship one with the app, also put it in `resources/color-schemes/` +
`resources/resources.qrc` (fallback build; for KF6 also as an alias under the
`/org.kde.kcolorscheme` prefix, like Kinoklip) and let `scripts/package_linux.sh` copy it to the
bundle's `usr/share/color-schemes/`.

**Add a widget style:** any Qt style plugin in the plugin path (`QStyleFactory::keys()`) is
listed automatically; for the bundle add its plugin to `PLUGIN_GLOBS` in
`scripts/package_linux.sh`.

**Map another icon to Breeze:** add a row to `kThemeIcons` in `ui/common/Icons.cpp` (check the
name exists: `ls /usr/share/icons/breeze/*/*/<name>.svg` in the container) and run
`test_theme`.

## How to extend

**Add a page** (e.g. the Color page later):
1. Subclass `kk::Page` (`ui/common/Page.h`), implement `pageId()` and, if it has toggleable panels,
   `hasPanel()/setPanelVisible()/isPanelVisible()` + `saveLayout()/restoreLayout()`.
2. In `MainWindow`'s constructor call `addPage(new MyPage(ctx), "view.page_<id>")`. The page
   action must exist in `ui/common/StandardActions.cpp` (and a shortcut in
   `resources/keymaps/resolve-windows.json`). For Color, replace the disabled placeholder button
   (set the page id in its `kPageSlots` row, as the Cut page did).
3. A page that edits the timeline reuses the Edit page's `TimelineController` (pass
   `editPage()->timeline()->controller()`, see `CutPage`), so tool, targets and clipboard are
   shared; give extra `TimelineWidget`s `TimelineViewOptions::keepsViewState = false`.

**Add a command-line mode** (e.g. `--render <project> <out.mp4>`):
1. Add fields to `CommandLineOptions` and an option in `parseCommandLine()`.
2. In `main()`, branch before the window is created (headless modes should not build a
   `MainWindow` at all) and return the exit code.

**Add an app-level command**: register it in `StandardActions.cpp`, then connect it in
`MainWindow::connectAppActions()` (window concerns) or in the module that owns the behaviour.
