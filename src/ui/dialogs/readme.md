# src/ui/dialogs — application dialogs

Owned by the panels agent.

| File | What |
|---|---|
| `ProjectSettingsDialog.h` | name, resolution presets + custom, frame rate (23.976 ... 60), sample rate (48 kHz fixed in v0.1); applied with `Project::setSettings()` = one undo step |
| `PreferencesDialog.h` | hardware acceleration (`playback/hwaccel`) with the working backends listed (`media::hwCapabilities()`, probed off the GUI thread), autosave minutes, system colours, UI scale |
| `KeyboardCustomizationDialog.h` | searchable command/shortcut table over `ActionRegistry`, `ShortcutRecorder`, conflict detection with reassign, reset one/all, import/export JSON, Cancel restores |
| `DialogActions.h` | `connectDialogActions(ctx, window, onPreferencesChanged)` — the one call the main window makes |

## Wiring
`MainWindow::connectAppActions()` calls `connectDialogActions()`, which connects
`file.project_settings`, `edit.keyboard_customization` and `edit.preferences` and applies the
stored hardware-acceleration preference to `media::setPreferredHwAccel()` at start-up (QSV maps
to VA-API, Intel's decode path on Linux). The callback restarts the autosave timer after the
Preferences dialog stored new values.

## Notes and known gaps
- **UI scale** is stored as `ui/scaleFactor` ("auto" or a factor like "1.25", constant
  `prefs::kUiScale` in `PreferencesDialog.h`). It needs `main()` to set `QT_SCALE_FACTOR` from
  it before creating `QApplication` (app owner) — until then the setting has no effect.
- **System colours** go through the `view.use_system_colors` action so the app's theme code
  stays the only place that applies themes.
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
