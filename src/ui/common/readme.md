# src/ui/common — shared UI infrastructure

Used by every panel and page. Depends on `core` only.

| File | What |
|---|---|
| `AppContext.h` | the struct of shared objects every panel receives: `Project*`, `SelectionModel*`, `ActionRegistry*`, `PlaybackEngine*` (null until src/playback exists) |
| `ActionRegistry.h` | every command as a `QAction` with a stable id; rebind/reset/import/export/conflict lookup; **keymap profiles** (implementation in `KeymapProfiles.cpp`, see below) |
| `StandardActions.h` | the catalogue: ids, texts, menus, icons; `standardMenus()` |
| `PanelFrame.h` | common panel chrome (title bar + tool area + body); `createPlaceholderPanel()` |
| `Page.h` | base class for main-window pages; `PanelId` for the interface-toolbar toggles |
| `Icons.h` | `kk::icon("play")` — own SVGs tinted from the palette (On state = accent) |
| `TimecodeFormat.h` | `timecode::format(frames, fps)` → `HH:MM:SS:FF`, `timecode::parse()` |
| `MediaImport.h` | import with user feedback; media file patterns/filters |
| `Resources.h` | `initResources()` — call in `main()` and in tests that need icons/keymap |
| `ShortcutFallback.h` | app event filter (MainWindow installs it): an unmatched key with a shifted US character (`Ctrl+Shift+{`) triggers the action bound to the unshifted form (`Ctrl+Shift+[`); never in text input or popups |

## Panel conventions
- Constructor `Panel(AppContext& ctx, QWidget* parent = nullptr)`; keep `AppContext&`.
- Derive from `PanelFrame` for a consistent look; put small tool buttons in the title bar with
  `addTitleBarAction(ctx.actions->action("..."))`.
- Never create model singletons; everything comes through `AppContext`.

## Action ids and owners
Behaviour is connected by the module that owns it:
`connect(ctx.actions->action("edit.split"), &QAction::triggered, this, &X::split);`

| Ids | Connected by |
|---|---|
| `file.new/open/save/save_as/import_media`, `file.quit`, `edit.undo/redo`, `view.page_*`, `view.toggle_*`, `view.single_viewer`, `view.fullscreen`, `help.*`; `settings.color_scheme.*` / `settings.style.*` (registered by app/Theme into the Settings submenus) | app (done) |
| `tool.*`, `trim.*`, `edit.split/delete/ripple_delete/cut/copy/paste/paste_insert/select_all/deselect_all/insert/overwrite/place_on_top/replace/fit_to_fill/append_at_end`, `timeline.*`, `clip.toggle_enabled/link/unlink`, `mark.*`, `transport.prev_edit/next_edit/go_to_start/go_to_end/prev_marker/next_marker` | timeline agent |
| `transport.*`, `view.fullscreen_viewer` | playback agent |
| `clip.add_keyframe/prev_keyframe/next_keyframe` | panels agent (Inspector) |
| `clip.toggle_keyframe_editor/toggle_curve_editor/delete_keyframe/copy_keyframes/paste_keyframes` | timeline (KeyframeLanes) |
| `file.project_settings`, `edit.keyboard_customization`, `edit.preferences` | panels agent (dialogs) |

`timeline.snapping`, `timeline.linked_selection` and the `tool.*` group are checkable and start
in their default state (snap on, linked on, select tool).

## Keymap profiles

Default keys come from the active keymap profile (plan.md 5.9 rule 1):

- **Built-in** profiles: `resources/keymaps/<id>.json` (read-only, compiled in). The default is
  `resolve-windows` (Resolve's Windows defaults, documented in docs/keymap-resolve-windows.md).
- **User** profiles: `<AppData>/keymaps/<name>.json`, id `user-<name>`; each has a `basedOn`
  built-in profile and stores only its overrides.
- Rebinding always changes the active profile: built-in profiles keep their overrides in
  QSettings (`shortcuts/<profile id>/<action id>`), user profiles in their file. Reset restores
  the base. Overrides saved before profiles existed (`shortcuts/<action id>`) are migrated to
  the default profile on start.
- API: `profiles()`, `profile(id)`, `activeProfile()` / `setActiveProfile(id)` (persisted in
  `keymap/profile`), `duplicateProfile()`, `renameProfile()`, `deleteProfile()`,
  `importProfile(path)` (creates a user profile), `exportProfile(id, path)` (complete keymap),
  `isProfileModified(id)`; signals `profilesChanged()`, `activeProfileChanged(id)`.
- The Keyboard Customization dialog shows a profile combo at the top (+ Duplicate / Rename /
  Delete), Import / Export work on profiles.

**Add a built-in profile:** drop `resources/keymaps/<id>.json` (with a `"name"`) into the
resources and list it in `resources/resources.qrc`; it must cover every action id (see the
test in `test_actionregistry`).

## How to extend

**Add a command:**
1. Add a row to `kActions` in `StandardActions.cpp` (category = menu id, optional icon name,
   checkable flag). Order in the table = order in the menu; `"-"` adds a separator.
2. Add its default shortcut to every built-in profile in `resources/keymaps/` (use `[]` for none) — the test
   `test_actionregistry` fails if an action is missing there or a shortcut is used twice.
3. Connect `triggered` in the owning module.

**Add an icon:** draw a 24×24 single-colour SVG (black strokes/fills; it is re-tinted at
runtime) in `resources/icons/<name>.svg`, list it in `resources/resources.qrc`, use
`kk::icon("<name>")`.

**Add a panel:** subclass `PanelFrame` in `src/ui/<area>/`, take `AppContext&`, place it in a
page (`src/app/pages/*`). If it should be toggleable, add a `PanelId` + toggle action.
