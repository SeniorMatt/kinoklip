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
| `Theme.h` | own dark Fusion palette + generated style sheet; "Use System Colors" option |
| `CommandLine.h` | `QCommandLineParser` → `CommandLineOptions` |
| `Screenshot.h` | `--screenshot <page> <out.png> [project]` (works with `QT_QPA_PLATFORM=offscreen`) |

## Start-up sequence (`src/main.cpp`)
1. `configureOpenGL()` — default `QSurfaceFormat` = OpenGL 3.3 core, `Qt::AA_ShareOpenGLContexts`
   (must happen before `QApplication`).
2. `QApplication`, organisation/app names (QSettings → `~/.config/cutwise/Cutwise.conf`),
   `cw::initResources()`, command line, `theme::apply()`.
3. `Project`, `SelectionModel`, `ActionRegistry` (+ `loadDefaultKeymap()` +
   `registerStandardActions()`), `AppContext`.
4. Non-interactive modes (screenshot) or `MainWindow::showRestored()`.

## Settings keys used here
`mainWindow/geometry`, `mainWindow/state`, `mainWindow/page`, `pages/<pageId>/...` (splitter
states and `visible/<panel>` flags), `paths/lastProjectDir`, `paths/lastMediaDir`. User
preferences (theme, hw acceleration, autosave) are in `core/Preferences.h`.

## How to extend

**Add a page** (e.g. the Color page later):
1. Subclass `cw::Page` (`ui/common/Page.h`), implement `pageId()` and, if it has toggleable panels,
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
