# src/platform — desktop integration

Small, UI-free helpers that adapt the app to the desktop it runs on. Usable from `src/app`
and `src/ui/**`; depends on Qt only.

| File | What it does |
|---|---|
| `ChildProcessEnv` | Gives programs we start (xdg-open, file managers) the user's environment back when running from the bundle (undoes `packaging/linux/apprun.sh`'s variables). |
| `FileDialogs` | The one entry point for every open / save / folder dialog. |
| `PortalFileChooser` | The XDG desktop portal's `org.freedesktop.portal.FileChooser` over D-Bus. |

## File dialogs

Every file dialog goes through `platform::runFileDialog()` or its wrappers
(`getOpenFileName(s)`, `getSaveFileName`, `getExistingDirectory`). Never call `QFileDialog`
directly. Current users: File > Open Project / Save Project As / Import Media, media pool
Import and Relink, keyboard profile import/export, Render Location, Preferences cache folder.

A request carries the title, a Qt-syntax filter (`"Media (*.mp4 *.mov);;All files (*)"`), an
optional start folder and suggested file name, and a `QSettings` key that remembers the folder
of the last choice (`paths/lastMediaDir`, `paths/lastProjectDir`, `paths/lastKeymapDir`,
`paths/lastRelinkDir`).

### Backend choice

1. **XDG desktop portal** (default when available): the desktop's own dialog — the KDE /
   Dolphin (KIO) file chooser on Plasma, GTK's on GNOME. `PortalFileChooser` speaks the
   protocol directly (the same calls Qt's `xdgdesktopportal` platform theme makes), so the
   app's platform theme stays `generic` and the palette, fonts, Breeze style and icons set by
   `src/app/Theme` are untouched. Availability is probed once, on the first dialog: the
   `org.freedesktop.portal.Desktop` name must be running or activatable and report a
   `FileChooser` `version` (0 = no file chooser backend). Folder pickers need version 3.
2. **Qt's `QFileDialog`** otherwise (no session bus, no portal, the call failed, or the portal
   went away while the dialog was open). Under the bundle's `generic` platform theme this is
   Qt's widget dialog; with `KINOKLIP_HOST_QT=1` on Plasma it is the KDE dialog of the host's
   platform theme.

`KINOKLIP_FILE_DIALOG=qt` forces Qt's dialog; `portal` never opens Qt's dialog after a failed
portal call (used by the tests; without any portal Qt's dialog is still used); `auto`/unset is
the default. Test hook:
`KINOKLIP_PORTAL_SERVICE=<bus name>` talks to another service (tests/app/test_filedialogs
runs a fake portal on a private bus via `dbus-run-session`).

Details:

- Filters are converted to portal filters with case-insensitive globs (`*.mp4` ->
  `*.[mM][pP]4`); the first one is preselected.
- The start folder is sent as `current_folder`; Save sends `current_name` (and `current_file`
  if it exists). The portal dialog confirms overwriting itself; callers still append the
  default suffix when the user typed none.
- The returned `file://` URIs become local paths. For sandboxed apps the portal returns
  document-portal paths (`/run/user/<uid>/doc/<id>/<name>`); they are ordinary readable files,
  but their folder is not remembered as the "last folder".
- The app's windows are blocked while the dialog is open (an invisible application-modal
  widget, as Qt does for native dialogs). The dialog is attached to our window through the
  portal's `parent_window`: `x11:<id>` on X11; on Wayland `wayland:<xdg-foreign handle>`
  via Qt's private `QDesktopUnixServices::portalWindowIdentifier()` (only in builds with
  Qt >= 6.9 private headers — the bundle — and only when the running Qt has the same minor
  version; otherwise the Wayland dialog opens unparented but works the same).
- If the application quits while a dialog is open, the portal request is closed.
