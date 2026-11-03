# How to use Kinoklip

Kinoklip is a free video editor for Linux. You put your clips on a timeline, cut them, add
titles, music and effects, and export a finished video. It is laid out like DaVinci Resolve and
uses the same keyboard shortcuts, so most Resolve tutorials for the Edit page also work here.
This guide starts with the basics and then shows, step by step, how to do the things people
ask about most.

Kinoklip is still an alpha version: save often and keep a copy of your original files.

## Install and start

1. Open the [download page](https://github.com/SeniorMatt/kinoklip/releases) and download
   **Kinoklip-x86_64.AppImage**. It is one file that contains the whole program.
2. Make it runnable, in one of two ways:
   - **With Gear Lever (easiest):** install [Gear Lever](https://flathub.org/apps/it.mijorus.gearlever)
     from your software centre, then open the AppImage with it. Kinoklip appears in your
     application menu, and Gear Lever keeps it updated.
   - **By hand:** right-click the file, choose **Properties**, open **Permissions** and tick
     **Is executable** (or run `chmod +x Kinoklip-x86_64.AppImage` in a terminal).
3. Start Kinoklip from the application menu, or double-click the file.

The first thing you see is the **Project Manager**, where you make your first project. The
[getting started guide](getting-started.md) takes it from there.

If Kinoklip does not start, your system may be missing FUSE (a small helper for AppImages). Run
`./Kinoklip-x86_64.AppImage --appimage-extract-and-run` in a terminal instead. Kinoklip needs a
graphics card or chip that supports OpenGL 3.3, which almost every computer from the last ten
years has.

## Help inside Kinoklip

Press **F1** in Kinoklip to open the article about what you are working on (for example the
green screen article while the Chroma Key settings are open). A small **?** button appears next
to panels and effects that have an article, and **Help > Guide** lists every article with a
search field.

## Start here

- [Getting started](getting-started.md): the pages, making a project, importing clips, editing
  on the timeline, a title and a transition, and exporting your video.

## How do I...?

- [Export a video for YouTube](export-for-youtube.md)
- [Remove a green screen](remove-a-green-screen.md)
- [Speed up or slow down a clip](change-clip-speed.md)
- [Fade audio in and out, and add music](fade-audio-and-add-music.md)

More articles are planned; see [the topic list](topics.md). Every keyboard shortcut is listed
in the [keymap](../keymap-resolve-windows.md).

