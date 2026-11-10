# How to use Kinoklip

Kinoklip is a free video editor for Linux. You put your clips on a timeline, cut them, add
titles, music and effects, and export a finished video. It is laid out like [DaVinci Resolve](https://www.blackmagicdesign.com/products/davinciresolve) and
uses the same keyboard shortcuts, so most Resolve tutorials for the Edit page also work here.
This guide starts with the basics and then shows, step by step, how to do the things people
ask about most.

Kinoklip is still a beta version: save often and keep a copy of your original files.

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
- [Fade to or from black](fade-to-black.md)
- [Add a transition, and hide a jump cut with Smooth Cut](transitions.md)
- [Freeze a frame](freeze-a-frame.md)
- [Test patterns, generators and ready-made looks](test-patterns-and-looks.md)
- [Sharpen, repair, damage and paint effects](sharpen-repair-and-damage-effects.md)
- [Move a timeline to or from another editor](move-a-timeline-to-another-editor.md) (OpenTimelineIO)
- [Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md)
- [Control Kinoklip from an AI assistant (MCP)](control-kinoklip-from-an-ai-assistant.md)
- [Zoom, crop and picture-in-picture](zoom-crop-and-picture-in-picture.md)
- [Fix choppy playback](smooth-playback.md)
- [Make a vertical video for Shorts, Reels and TikTok](vertical-video.md)
- [Blur or hide part of the picture](hide-part-of-the-picture.md)
- [Add subtitles](add-subtitles.md)
- [Separate the audio from the video](separate-audio-from-video.md)
- [Animate with keyframes](keyframe-animation.md)
- [Make quiet and loud clips the same volume](even-out-clip-volumes.md)
- [Put text over a clip on the Compositing page](compositing-page.md)
- [Colour correct a clip](colour-correct-a-clip.md) (the Color page)
- [Change the colour of one object](change-the-colour-of-one-object.md)
- [Use a LUT](use-a-lut.md)
- [Use the shortcuts of your old editor](use-the-shortcuts-of-your-old-editor.md) ([Vegas Pro](https://www.vegascreativesoftware.com),
  [Premiere Pro](https://www.adobe.com/products/premiere.html), [Final Cut Pro](https://www.apple.com/final-cut-pro/), Avid, [Kdenlive](https://kdenlive.org), [Shotcut](https://shotcut.org))

More articles are planned; see [the topic list](topics.md). Every keyboard shortcut is listed
in the [keymap](../keymap-resolve-windows.md); the other editors' presets are in
[keymaps](../keymaps/readme.md).

