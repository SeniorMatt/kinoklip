# Move a timeline to or from another editor

You will send your timeline to [DaVinci Resolve](https://www.blackmagicdesign.com/products/davinciresolve) (or another editor) and bring one back, using
an **[OpenTimelineIO](https://opentimeline.io)** file (`.otio`). The file holds the cuts, not the pictures: tracks, where
each clip sits, which part of the file it plays, gaps, dissolves, speed changes and markers.
The video and audio files themselves stay where they are.

## Send your timeline out

1. Open the timeline you want to send on the **Edit** page.
2. Choose **File > Export > Timeline as OpenTimelineIO (.otio)...**.
3. Pick a folder and a name, and press **Save**.

In the other editor, import the file as a timeline (in Resolve: **File > Import > Timeline**).
It looks for the media at the same places as on this computer. If the files are on another
computer or another drive, the other editor asks you to relink them.

What is written: the video and audio tracks, clips with the part of the file they play, gaps,
transitions between touching clips (a cross dissolve as an ordinary dissolve, every other Kinoklip transition as a custom
transition that carries its name and settings, so another Kinoklip reads it back as it was), clips at another speed (a speed ramp is written as its
average speed), disabled clips, timeline markers and clip markers, and the timeline's starting
timecode.

What is not written: subtitle tracks, effects, colour grades, transforms, fades at the start
or end of a clip, and audio levels. Titles, solid colours, generators and compound clips
arrive as offline clips with their names. Linked video and audio are two separate clips in the
file.

## Bring a timeline in

1. Choose **File > Import > Timeline...**.
2. Pick the `.otio` file.

A new timeline appears in the media pool and opens in a tab. Files the timeline uses are added
to the media pool in the bin you are looking at. If the project already has a file, it is not
added a second time.

If a file cannot be found, Kinoklip still builds the timeline: the clip is shown as offline,
with its name, and the message after the import lists the missing files. Right-click an offline clip in the media pool and choose **Relink
Offline Media...** to point them at the right folder.

After the import, a message lists what Kinoklip left out, for example effects it has no
equivalent for, nested sequences, transitions of other editors that Kinoklip does not know (they become cross dissolves) and
extra information other programs stored in the file. The rest of the timeline is imported
anyway.

One Undo (**Ctrl+Z**) takes the whole import away, the new timeline and the media it added.

## Frame rate

The new timeline runs at the frame rate stored in the file. If that differs from the project's
rate, the timeline keeps its own rate; change it later in the timeline settings if you want.

## If something goes wrong

- **The import says the file is not an OpenTimelineIO timeline.** The file may be an older
  `.otioz` bundle or something else saved as `.otio`. Ask the other editor to export a plain
  `.otio` file.
- **Every clip is offline.** The file points at folders that do not exist here. Right-click in the media pool and use
  **Relink Offline Media...**.
- **A clip plays the wrong part of its file.** Clips with a speed change are read from the file
  as: length on the timeline, speed factor and start in the file. Another program may store a
  speed change differently; tell us about it with Help > Report a Bug.
