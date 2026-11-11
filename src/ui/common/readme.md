# src/ui/common — shared UI infrastructure

Used by every panel and page. Depends on `core` only.

| File | What |
|---|---|
| `AppContext.h` | the struct of shared objects every panel receives: `Project*`, `SelectionModel*`, `ActionRegistry*`, `PlaybackEngine*` (null until src/playback exists) |
| `ActionRegistry.h` | every command as a `QAction` with a stable id; rebind/reset/import/export/conflict lookup (scope aware: window, `page:<id>`, `widget:<name>`); **keymap profiles** (implementation in `KeymapProfiles.cpp`, see below) |
| `VimInput.h` | the vim key layer (spec 62): `VimMachine` (counts, sequences, NORMAL / VISUAL; asks a `VimHost`, never edits) and `VimInput`, the application event filter that is active only with the keyboard profile `vim` and only while a widget with the property `kkVimScope` (timelines, viewers) has the focus and is not a text field |
| `StandardActions.h` | the catalogue: ids, texts, menus, icons; `standardMenus()`; `describeStandardActions()` gives each command its one-line description from `ActionDescriptions.cpp` (status tip; the menu reference, spec 63, is written from it) |
| `PanelInfo.h` | `setPanelInfo(widget, name, description)`: the name and one-line description of a panel or page area, read by the menu reference (spec 63) |
| `MediaManagement.h` | proxy / optimized media and render cache glue (created in `main()`, `AppContext::media`): `mediapool.*`, `playback.proxy_*`, `playback.use_*`, `playback.render_cache_*`, `clip.render_cache_output` commands; ProxyQueue + RenderCacheManager + a shared background Compositor; proxy map for every viewer, cache view for program playback, per-item status for the media pool |
| `PanelFrame.h` | common panel chrome (title bar + tool area + body); `setPanelKey()` names the panel for the in-app help (`panel:<key>`, src/ui/help); `createPlaceholderPanel()` |
| `Page.h` | base class for main-window pages; `PanelId` for the interface-toolbar toggles |
| `SchemeColors.h` | `kk::schemeColors()`: every colour of custom-painted widgets (clips, badges, playhead, lanes, keyframes, overlays...) derived from the active colour scheme's roles; `SchemeColorsNotifier::changed()` after a scheme switch. Never hard-code a colour in a widget: add a role here |
| `UiAnimation.h` | `kk::uianim`: the global switch of the small UI animations (prefs `ui/animations`, `ui/animationSpeed`, KDE's animation speed; off offscreen unless `KK_ANIMATIONS`), standard durations (`duration(Kind)`, 120-220 ms) and easing (OutCubic moves, light OutBack pops), the clock (`setClockForTesting()`), `PlayheadGlide`, `Notifier::changed()` |
| `DialogSizing.h` | `kk::dialogsizing::fit` (minimum and opening size of a dialog from its content, capped to the screen), `restoreGeometryIfFits`, `PageScrollArea` (a scroll area that asks for its page's size; spec 73) |
| `SpringLoad.h` | `kk::SpringLoad`: makes a button spring-loaded (spec 71): a drag resting on it for `kSpringLoadDelayMs` clicks it, with a highlight and a sweeping fill as the countdown; the page bar and the interface bar's buttons of panels that take drops (spec 74) use it |
| `Icons.h` | `kk::icon("play")` — own SVGs tinted from the palette (On state = accent) |
| `TextFit.h` | text in parts the user cannot resize never runs off its widget (docs/coding-style.md 5b, spec 23.20 - 23.23): `fitListWidthToItems()` (dialog section lists as wide as their widest item, refit on font / style change), `elideListItems()` (capped lists elide, full text as the tooltip), `setElidedText()` (labels and tool buttons in fixed-width strips) |
| `TimecodeFormat.h` | `timecode::format(frames, fps)` → `HH:MM:SS:FF`, `timecode::parse()` |
| `TimecodeEntry.h` | typed timecodes (spec 2.10): `timecode::parseTyped()` (calculator-style digits, + / - offsets) and the application key filter `TimecodeEntry` collecting them while the timeline has focus (EditPage wires it) |
| `TimelineCommandSuspension.h` | a page without the Edit page timeline (Media, Compositing, Deliver, ...) creates one in its constructor: the timeline's editing commands are disabled while the page is shown (spec 42b.22, 42b.26), minus the ids the page keeps |
| `MediaImport.h` | import with user feedback (a single-file project copies the files in first through `AppContext::mediaStore`); `storeItemsInProject()` (relink, Store in Project File); `isStoredInProject()` / `isLinkedFile()` / `mediaLocationText()`; media file patterns/filters |
| `FolderImport.h` | Import Media Folder (File menu, Media Pool menus and folder drops, storage browser): scan + probe on a worker thread (`media/FolderScan.h`, `MediaPool::prepareFile()`), the single-file store, then one undo step that creates the bins and adds the items; cancel rolls back; one summary dialog; `takeFolders()` splits dropped folders from files |
| `MediaStore.h` | where imported media go: the interface `app/ProjectController` implements for single-file projects (`AppContext::mediaStore`, null = linked) |
| `ProgressTask.h` | `runWithProgress()`: a long file operation on a worker thread behind a window-modal progress dialog with Cancel (imports, Pack, Unpack, Compact, Verify, Project Manager export); a test hook presses Cancel |
| `Resources.h` | `initResources()` — call in `main()` and in tests that need icons/keymap |
| `PanelSplitter.h` | `QSplitter` for every split between and inside panels: handles that highlight on hover / press and never trigger Breeze's mouse-grabbing splitter proxy (see "Wayland") |
| `DragAndDrop.h` | `execCopyDrag()`, `logDropEvent()`, the `kk.ui.dnd` logging category; `DragMime.h` holds the MIME contract between drag sources and drop targets |
| `ShortcutFallback.h` | app event filter (MainWindow installs it): an unmatched key with a shifted US character (`Ctrl+Shift+{`) triggers the action bound to the unshifted form (`Ctrl+Shift+[`); never in text input or popups |
| `ListKeyGuard.h` | app event filter (MainWindow installs it): a focused list or tree view (Effects library, media pool, Edit Index, bins, node library) claims Delete, Backspace, Ctrl+A, the arrows, Home / End and Page Up / Down through ShortcutOverride, so they act on the list instead of the timeline (spec 10.11); a view opts out with the property `kkTimelineKeys` |

## Panel conventions
- Constructor `Panel(AppContext& ctx, QWidget* parent = nullptr)`; keep `AppContext&`.
- Derive from `PanelFrame` for a consistent look and call `setPanelKey()` with the panel's
  help key (src/ui/help/readme.md); put small tool buttons in the title bar with
  `addTitleBarAction(ctx.actions->action("..."))`.
- Never create model singletons; everything comes through `AppContext`.

## Wayland

Plasma's default session is Wayland, and Qt Wayland enforces rules X11 never did:

- **No explicit mouse grabs.** `QWidget::grabMouse()` / `QWindow::setMouseGrabEnabled()` only
  work for popups; anything else prints "This plugin supports grabbing the mouse only for popup
  windows". Use the implicit grab of a press: the widget that got the press receives every move
  and the release. Drag-to-change fields (`ValueField`) measure motion relative to the press
  position, never warp the pointer (`QCursor::setPos()` does nothing on Wayland). Third-party
  grabs we neutralise: Breeze's splitter proxy (`PanelSplitter` eats the hover events that would
  show it) and QMenuBar's grab when the open menu's title is clicked (`MainWindow`'s
  `MenuBarGrabGuard` closes the menu first).
- **Windows cannot position themselves.** `move()` on a top-level is ignored; to let the user
  move a window from inside it (a panel header), call `QWindow::startSystemMove()` while the
  button is still pressed (the Keyframes panel header does).
- **Drag and drop** (below) needs the serial of the press that started it.

## Drag and drop

Payloads are defined in `DragMime.h`; sources and targets never parse each other's widgets.

Sources: start the drag from the press/move handlers while the left button is held (Wayland's
`wl_data_device.start_drag` needs the press serial; never from a timer or after the release),
offer **Copy only** (`execCopyDrag()`, or `supportedDragActions()` = `Qt::CopyAction` and
`setDefaultDropAction(Qt::CopyAction)` for item views; with Move an accepting target made
`QAbstractItemView` delete the dragged rows).

Targets: answer every `DragEnter` **and** `DragMove` with `acceptProposedAction()` or
`ignore()` (Qt Wayland turns each answer into `wl_data_offer.accept` + `set_actions`), log
with `logDropEvent()`, and never open a modal dialog inside `dropEvent()` (read the payload,
accept, then do the work from the event loop: `QTimer::singleShot(0, ...)`). Files from file
managers go through `platform::hasDroppedFiles()` / `platform::droppedLocalFiles()`
(src/platform), which also handle the formats Qt Wayland loses.

| Source | Payload | Targets |
|---|---|---|
| media pool items (grid / list) | `MediaItems` | timeline, bins tree (move to bin) |
| source viewer picture | `SourceRange` | timeline |
| effects library | `Effect` | timeline (clip, edit point, new clip), inspector |
| file managers, desktop | `text/uri-list`, `application/x-kde4-urilist`, `application/vnd.portal.filetransfer` | media pool, bins tree, timeline |

Diagnosing a drag on a user's machine: `QT_LOGGING_RULES="kk.ui.dnd.debug=true;kinoklip.drop.debug=true"`
logs every drag start, enter, move and drop with the formats and the accept state; with
`WAYLAND_DEBUG=1` the `wl_data_device` traffic (`start_drag`, `enter`, `drop`) shows what the
compositor did. Tests: `tests/timeline/test_timelinedrop.cpp`,
`tests/panels/test_droptargets.cpp` (synthetic events, every target), `tests/app/test_wayland.cpp`
(real drags under weston, see `scripts/wayland_session.sh`).

## Action ids and owners
Behaviour is connected by the module that owns it:
`connect(ctx.actions->action("edit.split"), &QAction::triggered, this, &X::split);`

| Ids | Connected by |
|---|---|
| `file.new/open/save/save_as/import_media`, `file.quit`, `edit.undo/redo`, `view.page_*`, `view.toggle_*` (incl. `view.toggle_keyframes`: EditPage docks / floats the Keyframes panel; `view.keyframes_panel_toggle_view`: flips its Keyframes / Curves view), `view.single_viewer`, `view.fullscreen`, `help.*`; `settings.color_scheme.*` / `settings.style.*` (registered by app/Theme into the Settings submenus) | app (done) |
| `tool.*`, `trim.*`, `edit.split/delete/ripple_delete/cut/copy/paste/paste_insert/select_all/deselect_all/insert/overwrite/place_on_top/replace/fit_to_fill/append_at_end`, `timeline.*`, `clip.toggle_enabled/link/unlink`, `mark.*`, `transport.prev_edit/next_edit/go_to_start/go_to_end/prev_marker/next_marker` | timeline agent |
| `transport.*`, `view.fullscreen_viewer` | playback agent |
| `cut.*` (smart edits, `cut.fast_review`, `cut.viewer_*` exclusive group; disabled until the Cut page shows) | src/ui/cut (`CutEditor`, `CutViewer`) and `app/pages/CutPage` |
| `audio.*` (`audio.add_bus`, `audio.reset_loudness`; `audio.float_viewer` enabled while the Audio page shows) | `app/pages/AudioPage` (widgets in src/ui/audio) |
| `clip.add_keyframe/prev_keyframe/next_keyframe` | panels agent (Inspector) |
| `clip.toggle_keyframe_editor/toggle_curve_editor/delete_keyframe/copy_keyframes/paste_keyframes` | timeline (KeyframeLanes) |
| `file.project_settings`, `edit.keyboard_customization`, `edit.preferences` | panels agent (dialogs) |

`timeline.snapping`, `timeline.linked_selection` and the `tool.*` group are checkable and start
in their default state (snap on, linked on, select tool).

## Keymap profiles

Default keys come from the active keymap profile (docs/archive/plan.md 5.9 rule 1):

- **Built-in** profiles: `resources/keymaps/<id>.json` (read-only, compiled in). The default is
  `resolve-windows` (Resolve's Windows defaults, documented in docs/keymap-resolve-windows.md),
  listed first; the presets of other editors (`avid-media-composer`, `final-cut-pro`,
  `kdenlive`, `premiere-pro`, `shotcut`, `vegas-pro`) are generated with their docs
  (docs/keymaps/) by `scripts/keymap-presets.py`, which reads the Resolve table and never
  changes it. A new command needs no row there: it keeps its Resolve key in every preset (or
  none where that preset uses the key); add a row only for a real counterpart.
- **Other editors' files**: `importProfile()` also reads Premiere Pro `.kys` and Final Cut Pro
  `.commandset` (XML plist) files through ForeignKeymaps.h (defensive parsers; command names
  mapped by an alias table; unknown entries counted as skipped, `ImportSummary`).
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
3. Add a one-line description of what it does to `ActionDescriptions.cpp` (read the slot first):
   it becomes the status tip and the "What it does" column of `docs/menu-reference.md`.
4. Connect `triggered` in the owning module.
5. Regenerate the menu reference with `scripts/menu-reference.sh` (the test `check_menu_reference`
   fails until you do).

**Add an icon:** draw a 24×24 single-colour SVG (black strokes/fills; it is re-tinted at
runtime) in `resources/icons/<name>.svg`, list it in `resources/resources.qrc`, use
`kk::icon("<name>")`.

**Add a panel:** subclass `PanelFrame` in `src/ui/<area>/`, take `AppContext&`, place it in a
page (`src/app/pages/*`). Call `setPanelInfo()` with its name and a one-line description (the
menu reference lists it) and regenerate `docs/menu-reference.md`. If it should be toggleable, add
a `PanelId` + toggle action.
