# Privacy

**Kinoklip never collects, stores or sends information about you or your computer unless you
explicitly choose to share it.**

## What the app does not do
- No telemetry and no analytics.
- No automatic crash reports.
- No update checks or other "pings" from the app itself. Gear Lever and AppImageUpdate look for
  new versions by reading the release files on GitHub; that happens in those tools, outside
  Kinoklip, when you ask them to.
- Kinoklip makes no network requests at all. Links (the user guide, a bug report) open in your
  web browser, and only when you click them.

## Reporting a bug
**Help > Report a Bug or Inconsistency...** helps you write a useful report:

1. You choose the kind of report: a bug, something that works differently from DaVinci Resolve,
   or an idea.
2. The dialog shows **exactly** the text that would be shared, so you can read every line before
   anything happens.
3. You choose:
   - **Open GitHub with This Info**: your browser opens GitHub's report form with that text
     filled in;
   - **Open GitHub Without It**: the same form, empty;
   - **Copy System Info**: the text goes to your clipboard, for reporting somewhere else;
   - **Cancel**: nothing happens.

Kinoklip itself sends nothing: you review the form on GitHub, can change or delete any of it, and
submit it yourself (you need a GitHub account). What you submit there is public and covered by
[GitHub's privacy statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement).

The text contains only versions and settings:
- the Kinoklip version (and the source commit, if the build has it) and how it is packaged
  (AppImage, Flatpak or built from source);
- the Linux distribution name and version, and the kernel release;
- the desktop, the session type (Wayland or X11) and Qt's platform plugin;
- the Qt and FFmpeg library versions;
- the OpenGL driver strings and the hardware decoding state, if Kinoklip already has them;
- the number of screens, and the size and scale of the main one;
- the widget style, colour theme and language.

It never contains your name, user name, computer name, home folder or any other path, file or
project names, your IP address or serial numbers. The text is read only when you open the dialog,
held in memory and never saved.

## What stays on your computer
Kinoklip keeps some data on your computer to work. None of it ever leaves your computer.
- **Settings**: `~/.config/kinoklip/Kinoklip.conf` (window layout, preferences, recent projects).
- **App data**: `~/.local/share/kinoklip/Kinoklip/` (autosaves for crash recovery, keyboard
  profiles, export presets, colour palettes, power bins).
- **Caches**: `~/.cache/kinoklip/Kinoklip/` (thumbnails, waveforms and other media caches), or the
  folder you chose in Preferences. Proxy and optimized media go where Preferences says. You can
  delete caches at any time; Kinoklip makes them again.
- **Projects**: wherever you save them (by default `~/Videos/Kinoklip Projects`).

## For contributors
Never add code that sends or collects user data without explicit, per-use consent. See
[contributing.md](contributing.md).
