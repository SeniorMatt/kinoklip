# Type text on with Write On

You will make a title appear letter by letter: H, He, Hel, Hell, Hello.

## The quick way

1. Open the **Effects** library, then **Titles**, and drag **Typewriter** onto a video track.
2. Select the clip and open the **Title** tab of the Inspector. Type your text in the **Text** box.
3. Play it. The text types on during the first second of the clip.

## Do it on any title

1. Select a title and open the **Title** tab. Find **Write On Start** and **Write On End** below
   **Line Spacing**.
2. Move to the first frame of the clip, set **Write On End** to 0 and click its keyframe button.
3. Move to the frame where the text should be complete, set **Write On End** to 1. A second key is
   added.

To erase the text instead, keep **Write On End** at 1 and key **Write On Start** from 0 to 1.

## What counts as a character

- Letters, digits, spaces and each emoji count as one. A letter with its accent, a flag or a
  family emoji is one character.
- Line breaks do not count.
- Hidden characters keep their place. The text does not move while it types, and a centred line
  stays centred on its full width.
- With nothing shown yet, a title with a background box still shows the empty box.

## On the Compositing page

The **Text** node has the same **Write On Start** and **Write On End** in its Inspector.
