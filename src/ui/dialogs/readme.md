# src/ui/dialogs — application dialogs

Owned by the panels agent.

| File | What |
|---|---|
| `ProjectSettingsDialog.h` | Resolve-style section list: Master Settings (resolution presets + custom, pixel aspect, timeline / playback frame rate, video monitoring), Image Scaling, Audio, General Options (name, autosave), Optimized Media and Render Cache (proxy / optimized resolution and format, render cache format, background caching, User mode options -> `Project::setCacheSettings()`, not undoable); applied with `Project::setSettings()` = one undo step |
| `PreferencesDialog.h` | Resolve-style window: System / User tabs, section list, the registered `SettingsPage`s of a section stacked under headings; Save / Cancel |
| `PreferencePages.cpp` | the standard pages + `applyStartupPreferences()` / `applyRuntimePreferences()` (see the table below) |
| `KeyboardCustomizationDialog.h` | searchable command/shortcut table over `ActionRegistry`, `ShortcutRecorder`, conflict detection with reassign, reset one/all, import/export JSON, Cancel restores |
| `DialogActions.h` | `connectDialogActions(ctx, window, onPreferencesChanged)` — the one call the main window makes |

## Wiring
`MainWindow::connectAppActions()` calls `connectDialogActions()`, which connects
`file.project_settings`, `edit.keyboard_customization` and `edit.preferences` and applies the
stored hardware-acceleration preference to `media::setPreferredHwAccel()` at start-up (QSV maps
to VA-API, Intel's decode path on Linux). The callback restarts the autosave timer after the
Preferences dialog stored new values.

## Preferences and where they go

| Section | Setting | Key | Applied by |
|---|---|---|---|
| System > Memory and GPU | still image cache | `system/stillCacheMiB` | `media::ImageLoader::setCacheLimitBytes()` (live) |
| | GPU processing mode | `system/renderBackend` | `KINOKLIP_RENDER=cpu` before the compositor exists (next start) |
| System > Media Storage | cache files location | `system/cacheDirectory` | `media::MediaCache::setCacheDirectory()` (live) |
| | proxy generation location, render cache size limit | `system/proxyDirectory`, `system/renderCacheLimitGiB` | `MediaManagement::preferencesChanged()` (live; optimized media and render cache live in the cache folder) |
| System > Decode Options | hardware decoding (+ API table) | `playback/hwaccel` | `media::setPreferredHwAccel()` (new decoders) |
| System > Audio I/O | output device | `system/audioOutputDevice` | `playback/AudioOutput` (first playback, i.e. next start) |
| User > UI Settings | colour theme, widget style (theme page), interface scale, animate timeline edits + animation speed | `ui/colorScheme`, `ui/widgetStyle`, `ui/scaleFactor`, `ui/animations`, `ui/animationSpeed` | theme actions (live), `main()` (next start), `uianim::reload()` (live) |
| User > Project Save and Load | autosave interval | `project/autosaveMinutes` | `AutosaveController` (live) |
| User > Editing | still / title+generator / transition durations, fast nudge, three-point editing, ripple delete gap, trim behaviour | `edit/*` (`core/EditBehaviour.h`) | read at each edit (live) |
| | timeline zooms around playhead / mouse | `edit/zoomAnchor` (`prefs::zoomAroundMouse()`) | timeline wheel zoom (timeline module) |
| User > Playback Settings | playback resolution (Full default / Half / Quarter / Automatic), pre/post-roll, return after play around, restart at end | `playback/previewQuality` (+ `previewQualityRevision`), `playback/*` (`PlaybackBehaviour`) | `PlaybackResolution::load()` (menu, viewer combos, engines) + `PlaybackEngine::setBehaviour()` (live) |
| User > Keyboard | opens Keyboard Customization | — | `edit.keyboard_customization` |

## Notes and known gaps
- **UI scale** is stored as `ui/scaleFactor` ("auto" or a factor like "1.25", constant
  `prefs::kUiScale` in `PreferencesDialog.h`). It needs `main()` to set `QT_SCALE_FACTOR` from
  it before creating `QApplication` (app owner) — until then the setting has no effect.
- **Settings pages are registered** (`ui/common/SettingsPages.h`, plan.md 5.9 rule 4): each
  page names its group ("system" / "user") and section; the dialog lists the sections.
  Standard pages: `registerStandardSettingsPages()`; the app's theme adds "appearance"
  (colour theme + widget style) to the "ui" section, which applies through the
  `settings.color_scheme.*` / `settings.style.*` actions so the theme code stays the only
  place that applies themes. To add a page: subclass `SettingsPage` (read values in the
  constructor, store them in `apply()`) and `registry.add({id, title, icon, order, factory})`
  with `group` / `section` set.
- **Frame-rate changes** keep clip positions in frames (see `Project::setSettings()`); the dialog
  warns when the timeline has clips.
- Keyboard changes apply live so they can be tried; Cancel restores the state from when the
  dialog opened. A recorded key replaces the command's *primary* shortcut and keeps secondary
  ones (e.g. Ripple Delete's Shift+Backspace).

## How to extend
- **A new preference:** add the key and typed accessors to `src/core/Preferences.h` (core
  owner), a control in the matching group of `PreferencesDialog`, load it in the constructor and
  store it in `apply()`. If something must react immediately, connect to
  `PreferencesDialog::preferencesChanged` (or extend the callback in `connectDialogActions()`).
- **A new project setting:** extend `ProjectSettings` (core), then add the control here and
  map it in `settings()` / `setSettings()`.
- **A new dialog opened from a menu:** add the action (see `src/ui/common/readme.md`), then
  connect it in `connectDialogActions()`.

## Clip Attributes (attrs branch)
`ClipAttributesDialog.h`: Video / Audio / Timecode tabs (spec 31.6 - 31.8);
`clipattributes::edit()` / `apply()` (one undo step on the media pool items),
`timelineTargets()`. `clip.attributes` is connected in `connectDialogActions()`: the media of
the selected timeline clips, else the media pool selection (published into the action's data
by MediaPoolPanel).
