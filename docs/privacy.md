# Privacy

**Kinoklip never collects, stores or sends information about you or your computer unless you
explicitly choose to share it.**

## What the app does not do
- No telemetry and no analytics.
- No automatic crash reports.
- No update checks or other "pings" from the app itself. [Gear Lever](https://flathub.org/apps/it.mijorus.gearlever) and [AppImageUpdate](https://github.com/AppImageCommunity/AppImageUpdate) look for
  new versions by reading the release files on GitHub; that happens in those tools, outside
  Kinoklip, when you ask them to.
- Kinoklip makes no network requests at all. Links (the user guide, a bug report) open in your
  web browser, and only when you click them.

## Working with other programs
**`kinoklip mcp`** lets an AI assistant that you set up yourself control Kinoklip with the same
commands as the command line. It is a local program: the assistant starts it, it talks to the
assistant over standard input and output only, it opens no network connection, and Kinoklip
sends nothing anywhere. What the assistant forwards to its own model (project and file names,
frames it asked to see) is decided by that assistant and its provider, not by Kinoklip. See
[Control Kinoklip from an AI assistant](guide/control-kinoklip-from-an-ai-assistant.md).

**Letting local tools control the open window** is a separate choice, **off by default**
(Preferences > System > Local Control). When you switch it on, Kinoklip listens on a socket in
your own runtime folder, made with permissions for your user only (other users of the computer
cannot connect, and Kinoklip checks who is connecting); it never listens on the network, and
Kinoklip itself still sends nothing anywhere. A program running as you can then edit the open
project, each change being one step you can undo, and a note in the window shows while one is
connected. Switch the option off and nothing listens any more.

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
  (AppImage, [Flatpak](https://flatpak.org) or built from source);
- the Linux distribution name and version, and the kernel release;
- the desktop, the session type (Wayland or X11) and Qt's platform plugin;
- the Qt and FFmpeg library versions;
- the OpenGL driver strings and the hardware decoding state, if Kinoklip already has them;
- the number of screens, and the size and scale of the main one;
- the widget style, colour theme and language.

It never contains your name, user name, computer name, home folder or any other path, file or
project names, your IP address or serial numbers. The text is read only when you open the dialog,
held in memory and never saved. (The log file of the next section is separate, and does name
files.)

## Exporting a log file
**Help > Export Log File...** saves what Kinoklip did in this session as one text file you can
attach to a bug report:

- The log is kept **in memory only** while Kinoklip runs (the last 20,000 lines or 4 MiB; older
  lines are dropped) and **written only when you ask**, to the file you choose. It is gone when
  Kinoklip closes. Kinoklip sends the file nowhere: you attach it to a report yourself, if you
  want to.
- The window says what the file contains and shows all of it before you save: the Kinoklip
  version, operating system, desktop (Wayland or X11), graphics card and OpenGL renderer (the
  same facts as a bug report), the decoder chosen for each media file, the playback statistics,
  and the warnings, errors and other messages of the session.
- Unlike a bug report's system info, the log **does name your media files** and some messages
  contain **folder paths**. Paths under your home folder are written as `~/...`, and your user
  name and computer name are removed. It has no project content, no picture or sound of your
  media, and none of your settings.
- `kinoklip log export --file <file>` does the same from the command line. Assistants connected
  through `kinoklip mcp` and programs using Local Control cannot run it.

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
