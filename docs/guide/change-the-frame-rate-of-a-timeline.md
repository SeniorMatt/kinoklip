# Change the frame rate of a timeline

You will change a timeline from, for example, 24 to 60 frames per second after the clips are
already on it, so that your 60 fps footage plays smoothly and keyframes have more frames to
move between. Nothing moves in time: every clip, marker, fade, transition, effect and keyframe
stays at the same second.

## Change it for one timeline

1. Open the timeline, then choose **Timeline > Timeline Settings**.
2. If **Use project settings** is ticked, untick it. The settings below it become editable.
3. Choose the new rate in **Timeline frame rate**. A note under the box says what will happen.
4. Click **OK** and confirm **Change Frame Rate**.

The project keeps its own frame rate; only this timeline (and the compound clips inside it that
ran at its old rate) changes. **Edit > Undo** takes the whole change back in one step.

## Change it for the whole project

1. Choose **File > Project Settings**.
2. Pick the new **Timeline frame rate**. When timelines that use the project settings have clips,
   a note under the box tells you they will be converted.
3. Click **OK** and confirm **Change Frame Rate**. One **Undo** step undoes it.

## What to expect

- A clip's start or end can move by at most half a new frame, because it has to land on a whole
  frame. Clips that touched still touch, and gaps stay gaps.
- Going to a higher rate loses nothing. Going to a lower rate (60 to 24) can merge two keyframes
  that land on the same frame; the one nearer its original moment is kept.
- Source in and out points are times, so every clip still shows the same part of its media.
- Effect settings counted in frames, such as the spacing of Motion Trails, are converted too.
- The timeline view keeps its zoom per second, and the playhead stays at the same time.

To do the same from a terminal on a saved project file, see
[Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md).
