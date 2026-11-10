# Effects that use the frames around a frame

This page lists the effects of version 1.2 that look at the frames before and after the one being
shown: trails and smears, a camera-style motion blur, and the repair effects for dust, flicker and
colors that drift. They sit in the effects library (**Toolbox > Effects**) in the folders named
below. Put one on a clip and change its settings in the **Inspector**.

A few things hold for all of them:

- They read the **same clip** at its own frame steps. Trimming, speed changes, reverse and freeze
  frames are respected: on a clip at double speed the "previous frame" is two source frames back.
  At the very start of the footage the first frame repeats instead of showing nothing, and next to
  a trimmed edge they use the frames you trimmed away.
- They look at no more than a few frames (at most six in all, however many are stacked), so
  playback stays smooth. If a neighbouring frame cannot be shown in time, the effect uses the
  nearest frame that is there.
- A paused frame, playback and an export show the same picture for the same frame. Nothing
  builds up from one frame to the next.
- Effects above one of these in the Inspector are applied to the neighbouring frames too, so a
  color correction before **Deflicker** counts in the brightness it measures.
- On an adjustment clip, and inside a Compositing page clip, they see the current frame only
  (the picture below is not drawn again at other frames), so they change nothing there; put them
  on the clip itself. **Patch Replacer** works anywhere.

## Temporal

- **Motion Trails.** Leaves a fading trail of the earlier frames behind everything that moves.
  **Trails** is how many frames are used, **Frame Spacing** how far apart they are, **Fade** how
  quickly the older ones fade away, **Trail Opacity** how strong the trail is. **Lighten** shows
  the brightest of the frames, which suits light objects on a dark background; **Average** blends
  them. Nothing is drawn in front of what moves.
- **Smear.** Averages the frame with the frames around it, for a soft smear along every
  movement. **Length** is how many frames on each side, **Direction** limits it to the frames
  **Behind** or **Ahead**, **Falloff** makes the farther frames count less and **Amount** mixes
  the smear with the plain picture.
- **Motion Blur.** Measures how the picture moves from the previous frame to this one and on to
  the next, and blurs along that movement, the way a camera shutter does. **Shutter Angle** is
  how long the shutter is open (180 degrees is the usual look, 360 degrees blurs over a whole
  frame) and **Quality** is the number of samples along the blur. It works well for objects that
  move steadily; where the picture changes without moving (a flicker, a fade) nothing is blurred.
  The movement is found by comparing small blocks of the picture, so very fast or very small
  motion, and things that appear from behind other things, can come out smeared or left sharp.

## Revival

- **Frame Replacer.** Replaces a damaged frame with the **Previous Frame**, the **Next Frame** or
  a **Mix of Both**. **Replace** is the share that is replaced: leave it at 100 and cut the clip
  so that only the bad frame has the effect, or keyframe **Replace** from 0 up to 100 and back at
  the bad frame.
- **Deflicker.** Evens out brightness that jumps from frame to frame by pulling each frame
  toward the average of the frames around it. **Time Window** is how many frames on each side
  are averaged (a longer window flattens slower flicker), **Strength** how much of the correction
  is applied, **Limit** the most the brightness may be changed. **Correct Colour Too** corrects
  each color channel on its own instead of only the brightness.
- **Automatic Dirt Removal.** Removes dust and hairs that show in one frame only. A spot counts
  as dirt when it differs clearly from the frames before and after (**Dirt Contrast**) while those
  two frames look alike (**Motion Tolerance**, so things that really move are kept). **Grow** adds
  a margin around what is found, **Strength** mixes the repair with the original. With **Time
  Window** 2 five frames are used, which also removes dust that stays for two frames. Something
  thinner than the distance it moves in one frame looks like dust to this effect: raise **Dirt
  Contrast**, or use **Dust Buster** on the part of the picture that has the dust.
- **Dust Buster.** The same repair, but only inside a region you place: **Region Shape**,
  **Center X / Y**, **Width**, **Height** and **Feather** (the repair fades in from the edge).
  **Repair** can be **Detected Dirt**, or **Whole Region**, which replaces everything inside the
  region with what the frames around show there. There is no on-screen handle for the region yet;
  type its position and size.
- **Patch Replacer.** Covers a blemish with a patch copied from another place in the same
  picture. **Source X / Y** is where the patch comes from, **Target X / Y** where it goes
  (pixels from the middle of the picture), **Width**, **Height**, **Patch Shape**, **Feather** and
  **Blend**. It uses only the current frame.

## Color

- **Color Stabilizer.** Holds exposure and white balance steady by matching every frame to a
  **Reference Frame** (a frame number counted from the start of the clip). **Match** can be
  **Exposure and White Balance**, **Exposure** only, or **White Balance** only (the brightness of
  each frame is kept). **Region X / Y / Width / Height** (percent of the picture) choose the
  part of the picture the colors are measured in, so a patch of sky or a grey wall can stand for
  the whole scene; keep it off moving people. **Strength** and **Limit** work as in Deflicker.
  The reference frame is read for every frame, so choose a frame inside the clip.

See also [Sharpen, repair, damage and paint effects](sharpen-repair-and-damage-effects.md) for
the other effects in the Revival folder, and [Steady a shaky clip](stabilize-a-shaky-clip.md).
