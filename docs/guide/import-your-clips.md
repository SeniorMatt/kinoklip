# Import your clips

Your clips have to be in the project before you can edit them. Kinoklip keeps them in the **media
pool**, on the left of the Edit and Media pages.

## Bring files in

Do one of these:
- Choose **File > Import > Media** (**Ctrl+I**) and pick your video, audio and picture files.
- Or drag the files from your file manager into the media pool.
- Or copy the files (or a picture, such as a screenshot) anywhere, click in the media pool and press **Ctrl+V**. A pasted picture is saved as a PNG in a **Pasted Media** folder next to your project (or in Kinoklip's data folder if the project is not saved yet).

Your clips now appear in the media pool. Kinoklip reads common camera and phone formats (MP4,
MOV, MKV, H.264, H.265, ...), audio files and pictures. A file with several audio streams, such as
a recording with game, microphone and chat sound, gets one audio clip per stream, on consecutive
audio tracks, when you put it on the timeline.

## Look at a clip without opening it

Move the pointer across a clip's thumbnail (or film strip) in the media pool: the viewer shows the
frame at that place, the left edge being the first frame and the right edge the end of the clip,
and goes back to what it showed when the pointer leaves. A picture shows itself. Nothing changes
while the viewer plays. Turn this **Live Media Preview** off in the media pool's **...** menu.

## Show everything in a file (Metadata panel)

The **Metadata** panel shows the usual details of the selected clip. Open its **...** menu and
turn on **Show All Metadata** to list everything the file contains: the container and each stream
(codec, colour range, transfer, frame rate and so on), all tags including the ones only a camera
writes (for example `com.dji.camera.ColorGammaSxS : D-Log`, which tells you the clip was filmed
in D-Log), chapters, and stream side data such as the rotation or HDR mastering values. Type in
the search field to narrow the list. Right-click a row, or press **Ctrl+C**, to copy its value or
its name and value; **Copy All** (right-click, or the **...** menu) copies the whole list. The
view is read only, and Kinoklip remembers that you turned it on.

## Put a LUT on a clip before you grade it

Footage shot in a log profile needs a LUT to look normal. Right-click the clip in the media pool
(or its thumbnail in the clips strip of the Color page) and choose **3D LUT**: pick a `.cube`
file from the list (the same folders as the Color page's LUT browser), **None** to take it away,
or **Browse...** for any other file. The LUT is applied before the grade's nodes, to every use
of that clip: the media pool thumbnail, the viewers of all pages, the render and the export.
**Edit > Undo** takes it back.

## The frame rate question

If your clips were filmed at another frame rate than the project (for example 25 or 30 frames per
second, where a new project uses 24), Kinoklip asks whether to change the project to match. Click
**Change**, so your video plays as smoothly as it was filmed. Once a timeline has clips, its frame
rate can no longer be changed this way; see
[Change the frame rate of a timeline](change-the-frame-rate-of-a-timeline.md) for that case.

## Files on the timeline, and files you lose

You can also drag files straight from your file manager onto the timeline. While you drag, an
outline shows where each clip lands and how long it is; when you drop, Kinoklip imports the files
into the media pool and puts the clips there.

In a **linked** project the media pool only points to your files, so do not move or rename them
afterwards. In a **single file** project the clips are inside the project; to get the original of
one back, right-click it in the media pool and choose **Save a Copy to Computer...**.

## Relink files that moved or were renamed

If the media pool shows offline clips, right-click one (or several) and choose **Relink Selected
Clips...**, or **Relink Offline Media...** for all of them, and pick the folder that holds the
files. Kinoklip then shows a table before it changes anything: the old file, the file it found,
how it found it and how good the match is. Clips it found nothing for are listed too.

Names do not have to be identical. Kinoklip matches, best first: the same name; the same name in
another case (`CLIP.MOV`); the same name with another extension (`clip.mp4` for `clip.mov`, for
files converted to another format); the name changed by your **Find and replace** (a regular
expression applied to the old name without its extension, for example `_v1$` replaced by `_v2`,
or `^(\w+)_v\d+$` replaced by `\1_final`); and, if you tick **Closest name**, the file with the
most similar name (a typo, another numbering, an added suffix). The percentage shows how similar.
A file is given to one clip only. Untick a row to leave that clip out, or double-click a row to
choose its file yourself. **Relink** applies everything as one step that **Undo** takes back.

Next: [Cut and arrange clips](cut-and-arrange-clips.md).
