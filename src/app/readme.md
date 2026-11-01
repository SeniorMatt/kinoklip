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
| `PageBar.h` | bottom strip: Media · Edit · Color (disabled) · Deliver |
| `pages/EditPage.h` | PLAN §4 Edit layout with nested `QSplitter`s; accessors for every panel |
| `pages/MediaPage.h` | browser · source viewer · metadata over the media pool |
| `ProjectController.h` | new / open / save / save as / import media + "save changes?" |
| `AutosaveController.h` | autosave every `prefs::autosaveMinutes()` (2) to `AppDataLocation/autosave/` |
| `Theme.h` | the real KDE Breeze look: widget style, KDE colour schemes, Breeze icons, panel style sheet; Settings > Color Theme / Style; "appearance" settings page |
| `ColorSchemeFile.h` | reads a KDE `.colors` file into a QPalette (fallback build without KF6) |
| `CommandLine.h` | `QCommandLineParser` → `CommandLineOptions` |
| `Screenshot.h` | `--screenshot <target> <out.png> [project]`: pages, `preferences`, `keyboard`, `menu-color-theme`, `menu-style` (works with `QT_QPA_PLATFORM=offscreen`) |

## Start-up sequence (`src/main.cpp`)
1. `configureOpenGL()` — default `QSurfaceFormat` = OpenGL 3.3 core, `Qt::AA_ShareOpenGLContexts`
   (must happen before `QApplication`).
2. `theme::initBeforeApplication()` (KIconTheme: bundled Breeze icons), `QApplication`,
   desktop file name `org.kinoklip.Kinoklip` (Wayland app_id), organisation/app names (QSettings → `~/.config/cutwise/Kinoklip.conf`),
   `kk::initResources()`, command line, `theme::setSessionOverrides()` + `theme::apply()`.
3. `Project`, `SelectionModel`, `ActionRegistry` (+ `loadDefaultKeymap()` +
   `registerStandardActions()` + `theme::registerActions()`), `SettingsPageRegistry`
   (standard pages + `theme::registerSettingsPage()`), `AppContext`.
4. Non-interactive modes (screenshot) or `MainWindow::showRestored()`.

## Settings keys used here
`mainWindow/geometry`, `mainWindow/state`, `mainWindow/page`, `pages/<pageId>/...` (splitter
states and `visible/<panel>` flags), `paths/lastProjectDir`, `paths/lastMediaDir`. User
preferences (theme, hw acceleration, autosave) are in `core/Preferences.h`.

## Theme system (the real KDE Breeze, the Kdenlive way)

Same approach as Kdenlive on Windows/macOS: the app ships the actual Breeze Qt style plugin,
the Breeze icons and the KDE colour schemes, so it looks identical on every system, and the
user can change all of it.

| Part | KF6 build (Arch container, release bundle) | Fallback build (Qt 6.4, no KF6) |
|---|---|---|
| Widget style | `QApplication::setStyle("Breeze")` (plugin `styles/breeze6.so`); Fusion if missing | Breeze if the host has a Qt 6.4 build of it, else Fusion |
| Colours | `KColorSchemeManager::activateSchemeId()`; schemes from `color-schemes/` in `XDG_DATA_DIRS` (bundle: `usr/share/color-schemes`) | `ColorSchemeFile` parses the same `.colors` files (copies in `resources/color-schemes/` + XDG dirs) into a QPalette |
| Icons | `KIconTheme::initTheme()` before `QApplication`: Breeze compiled into libKF6BreezeIcons, drawn by KIconEngine which recolours them from the palette | `QIcon::setThemeName("breeze"/"breeze-dark")` by palette lightness if installed, else our SVGs |
| Chrome | style sheet generated from the palette (`Theme.cpp: styleSheet()`): only backgrounds/borders of our own panels and bars; buttons, tabs, menus, focus and selection are left to Breeze | same |

Defaults: style `Breeze`, scheme `BreezeDark`. Settings keys: `ui/widgetStyle`,
`ui/colorScheme` (`""` = "Default (System)"; the old `ui/useSystemColors=true` is migrated to
it). Command line (this run only, nothing stored): `--widget-style Fusion`,
`--color-scheme BreezeLight|system`.

**User interface.** Settings > Color Theme and Settings > Style are *data*: StandardActions
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

**Editor visuals.** Timeline lanes, headers, ruler, separators and overlays derive from the
palette (`TimelinePainter::setStyle()`, re-run on `QEvent::PaletteChange`); clip colours are
content colours that read on light and dark schemes. Video viewer surrounds stay dark grey
(neutral for judging pictures) in every scheme.

**Add a colour scheme:** drop a KDE `.colors` file into `~/.local/share/color-schemes/`
(or any `color-schemes/` dir in `XDG_DATA_DIRS`); it appears in Settings > Color Theme on the
next start. To ship one with the app, also put it in `resources/color-schemes/` +
`resources/resources.qrc` (fallback build) and let `scripts/package_linux.sh` copy it to the
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
   `resources/keymap_default.json`). For Color, replace the disabled placeholder button.

**Add a command-line mode** (e.g. `--render <project> <out.mp4>`):
1. Add fields to `CommandLineOptions` and an option in `parseCommandLine()`.
2. In `main()`, branch before the window is created (headless modes should not build a
   `MainWindow` at all) and return the exit code.

**Add an app-level command**: register it in `StandardActions.cpp`, then connect it in
`MainWindow::connectAppActions()` (window concerns) or in the module that owns the behaviour.
