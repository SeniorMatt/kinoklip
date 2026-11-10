# Test patterns, generators and ready-made looks

This page lists what was added to the effects library for version 1.2: more **generators**
(**Toolbox > Generators**), ready-made adjustment clips (**Toolbox > Effects**) and new effects
for the picture. Everything is placed the usual way: drag it onto the timeline, or onto a clip
for an effect, and change its settings in the **Inspector**.

## Test patterns

Generators make a picture without any footage. The test patterns are for checking a monitor,
a cable or a conversion, so their colours are exact.

- **100mV Steps.** A grey scale in eight steps of 100 mV, from 0 to 700 mV of the video
  signal. **Orientation** turns it vertical.
- **EBU Color Bar.** The eight vertical bars of the European broadcast pattern: white at 100
  %, yellow, cyan, green, magenta, red and blue at 75 %, then black.
- **BT.2111 Color Bar HLG Narrow, PQ Narrow and PQ Full.** The high dynamic range pattern of
  [ITU-R BT.2111](https://www.itu.int/rec/R-REC-BT.2111): 100 % bars on top, 75 % (HLG) or 58 %
  (PQ) bars below them, a stair of grey steps, a ramp, black levels and the Rec. 709 colours at
  the bottom corners. The three names are the three signal types of the Recommendation. The
  values are written from its tables. A Kinoklip picture holds full-range 0 - 100 % values, so
  the narrow range patterns are drawn as the percentage each code value stands for (the code
  value 721 is the 75 % white), and the "-7 %" and "109 %" steps, which no full-range picture can
  hold, show as 0 % and 100 %.
- **YCbCr Ramp.** Ramps of the luma Y' and the two colour-difference signals Cb and Cr (Rec. 709),
  one above the other; **Channel** shows only one of them.

## Other generators

- **Window.** A white rectangle or ellipse on black: **Width**, **Height**, **Center X / Y**,
  **Softness** for a soft edge, and both colours.
- **Noise Gradient.** Smooth, cloudy noise between two colours. **Scale** is the size of the
  clouds, **Detail** how many finer layers are added, **Contrast** how far the colours are
  pushed apart. Keyframe **Evolution** to make the clouds move.
- **Grid.** Lines over a see-through background: **Columns** and **Rows**, the line colour and
  width, and every n-th line drawn thicker. Put it above a clip to line things up. A
  **Background Opacity** above 0 fills the cells.

## Ready-made looks (Toolbox > Effects)

Each of these is an adjustment clip, so it acts on every track below it and you can change each
part afterwards.

- **Colored Border.** A colored frame around the picture (the **Colored Border** effect).
- **Binoculars.** The picture seen through two round lenses; the masks move and resize in the
  viewer.
- **Night Vision.** A green, grainy picture with scanlines and dark corners, made of a Duotone,
  Film Grain, Scanlines and Vignette.
- **Composition Clip.** A see-through clip with an empty composition, for building a node
  graph on the [Compositing page](compositing-page.md) that is not attached to footage.

## New effects

- **Color:** **Despill** (removes a green, blue or red cast), **Gamut Limiter** (pulls colors
  that lie outside a smaller gamut back inside it), **Contrast Pop** (local contrast),
  **Chromatic Aberration Fix** (scales the red and blue to line them up with the green; **Fringe
  Removal** takes the color out of edges), **False Color** (paints by brightness, for checking
  exposure) and **Dehaze**.
- **Key:** **Alpha Matte Shrink and Grow** (moves the transparent edge in or out, with an edge
  blur).
- **Transform:** **Transform** (position, scale, rotation, pivot and opacity as an effect).
- **Distort:** **Dent** (bows the picture out or in: round, along one direction, or square).
- **Texture:** **Fast Noise** (noise over the picture, with a blend mode; **Drift Speed** moves
  it by itself).
- **Stylize:** **Colored Border**.
