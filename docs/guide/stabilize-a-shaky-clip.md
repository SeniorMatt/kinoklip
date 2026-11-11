# Steady a shaky clip

You will take a clip filmed by hand, with the camera trembling or swaying, and make the picture
calmer. Kinoklip measures how the camera moved during the clip, then moves the picture the
opposite way, frame by frame. It works like the Stabilization section of
DaVinci Resolve's Inspector.

Only clips of video files can be stabilized. A still picture, a title or a frozen frame has no
camera movement to measure.

## Stabilize a clip

1. Click the clip on the timeline to select it, and open the **Video** tab of the Inspector.
2. Find the **Stabilization** section, between **Speed Change** and **Retime and Scaling**.
3. Choose the **Mode**, which says what kind of camera movement is corrected:
   - **Perspective**: everything, including the picture tilting in depth. The most powerful
     choice, and the one to start with.
   - **Similarity**: pan, tilt, rotation and zoom, but no tilting in depth. Steadier when
     Perspective makes the picture wobble.
   - **Translation**: pan and tilt only. For a camera that shakes up, down and sideways but
     does not turn.
4. Click **Stabilize**. A progress bar shows while Kinoklip looks at the clip. You can keep
   editing; click **Cancel** to stop, and the clip stays as it was.
5. When the message says **Stabilized**, play the clip. Click the switch in the section header
   to compare the clip with and without the correction.

The result is saved in the project with the clip, and **Ctrl+Z** undoes the whole step.

## Adjust the result

These controls work at once, without measuring again:

- **Smooth** (0 to 1): how calm the new camera path is. Low values follow the camera closely and
  only take out small shakes. High values give a very steady picture, as if the camera were on a
  slow, smooth rail; a slow pan stays, a jerk goes. **0** corrects nothing.
- **Camera Lock**: removes all camera movement, like a camera on a tripod. **Smooth** and
  **Cropping Ratio** do nothing while it is on.
- **Zoom**: the correction moves the picture, which leaves empty edges. With **Zoom** on,
  Kinoklip enlarges the picture just enough to hide them in every frame of the clip. With it off,
  you see the moving edges and the picture keeps its size.
- **Cropping Ratio** (0.25 to 1): how far **Zoom** may enlarge the picture. At 1 it may enlarge
  up to twice; at 0.25 only a quarter more, and strong corrections then show some edge.
- **Strength** (-1 to 1): how much of the correction is applied. 1 is all of it, 0.5 half, 0
  switches the correction off, and a negative value shakes the picture the opposite way.

Only changing the **Mode** needs a new measurement: press **Stabilize** again, and until you do
the old measurement is not used (the section says so).

## Good to know

- The measurement follows the parts of the picture that move together, so a person walking
  through the scene does not pull the picture along. Pictures with little detail (a plain sky, a
  white wall) are hard to follow; Kinoklip leaves such stretches as they are and tells you how
  many it could not match.
- You can trim, split, slow down or reverse the clip after stabilizing it: the correction belongs
  to the frames of the video file, so each frame keeps its own. Frames you stretch the clip to
  that were not part of the measurement are shown uncorrected; press **Stabilize** again.
- The correction is applied before the clip's own **Transform** and **Cropping**, so you can still
  move, zoom and crop the clip as you like.
- From the command line or an AI assistant, use `clip stabilize` (see
  [Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md)).
