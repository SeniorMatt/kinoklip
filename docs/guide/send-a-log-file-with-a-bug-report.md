# Send a log file with a bug report

When something goes wrong, the most useful thing you can add to a bug report is the **log
file**: a text file that says what Kinoklip did during your session, such as which decoder it
used for each of your video files, whether the graphics card was used, and every warning or
error it printed. It often shows the cause at once, especially for playback that stutters, a
picture that stays black, or a crash.

Kinoklip keeps this log in memory while you work. **Nothing is written to your disk and nothing
is sent anywhere until you save the file yourself.**

## Save the log file

1. Reproduce the problem first, in the same session if you can (the log only holds what happened
   since Kinoklip was started, and the last 20,000 lines of it).
2. Choose **Help > Export Log File...**.
3. The window says what the file contains and shows the whole text, so you can read it before
   you save it. Scroll through it if you like.
4. Click **Save Log File...**, choose where to save it (the suggested name is
   `kinoklip-log-` and today's date, ending in `.txt`) and save.

## What is in the file

- The Kinoklip version, your operating system, the desktop (Wayland or X11), your graphics
  card and the OpenGL driver it uses.
- The decoder Kinoklip chose for each media file (graphics card or software).
- The playback statistics: frames shown and dropped, and how long decoding and drawing took,
  once a second while the video played.
- The warnings and errors of the session.
- Other short messages Kinoklip printed.

It also names the **media files** you used, and some messages contain **folder paths**. Paths
under your home folder are written as `~/...`, and your user name and computer name are
removed. The file has no project content, no picture or sound from your media, and none of your
settings.

## Attach it to your report

1. Choose **Help > Report a Bug or Inconsistency...** and continue to GitHub's report form (see
   [Getting started](getting-started.md)).
2. Drag the log file onto the form, into the box for screenshots and files. GitHub uploads it.
3. Before you submit, you can open the file in any text editor and delete anything you would
   rather not share. What you submit on GitHub is public.

Kinoklip never sends the file by itself. If playback is the problem, you can also try
[Fix choppy playback](smooth-playback.md) first.

## From a script

`kinoklip log export --file kinoklip-log.txt` writes the log of that run of the command to a
file. It is a command for you at the keyboard: assistants connected through `kinoklip mcp` and
programs using Local Control cannot run it, because the file can name your folders.
