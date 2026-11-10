# Sharpen, repair, damage and paint effects

This page lists the second group of effects added to the effects library for version 1.2. They
sit in the folders named below (**Toolbox > Effects**). Put one on a clip, or on an adjustment
clip to act on every track below it, and change its settings in the **Inspector**. Anything that
looks random (dirt, tape noise) is the same on every playback and every export of the same frame;
the **Seed** setting picks another pattern.

## Sharpen

- **Sharpen Edges.** Sharpens only the real edges and leaves flat areas and fine texture alone.
  **Edge Threshold** is how strong a step must be to count as an edge; **Halo Limit** stops the
  bright and dark rims that over-sharpening makes.
- **Soften and Sharpen.** Softens fine texture such as skin pores (**Soften**) while sharpening
  the larger edges (**Sharpen**). **Texture Limit** decides what still counts as fine texture.

The plain **Sharpen** and **Unsharp Mask** effects in the Blur folder stay as they were.

## Texture

- **Texture Pop.** Brings out the fine texture of surfaces, such as fabric, stone and skin
  detail, without changing the large tones. **Highlight Protection** keeps bright areas from
  clipping.
- **Detail Recovery.** Lifts deep shadows and calms bright highlights while keeping the detail
  inside them, and can add more detail on top (**Detail**).
- **Analog Damage.** A worn videotape: **Line Jitter**, **Color Bleed**, **Noise**, a **Tracking
  Error** band at the bottom and **Scanlines**.
- **Film Damage.** Old film: **Dirt**, **Scratches**, a dark **Vignette**, **Softness**, **Gate
  Weave** and **Flicker**.
- **JPEG Damage.** Makes the picture look like a badly compressed JPEG: visible blocks, steps in
  smooth areas and blotchy color. Lower **Quality** is worse; **Block Size** changes the grid.

## Revival

- **Deband.** Smooths the visible steps in skies and gradients and dithers them away.
  **Threshold** is the biggest step it will smooth, so real edges stay.
- **Dead Pixel Fixer.** Finds single stuck bright or dark pixels, which differ from all their
  neighbours, and replaces them with the average of the neighbours. **Defect Size** is the width
  of the defects to look for; **Detect** limits it to bright or dark ones.

## Refine

- **Beauty.** Smooths skin and other blotchy areas but keeps edges, with **Smoothing**, a
  **Smoothing Size**, an **Edge Sensitivity** and **Detail Recover** that gives back a share of
  the fine texture so the result does not look plastic. It does not find faces; switch on **Skin
  Tones Only** to limit it to skin-colored areas.

## Stylize

- **Watercolor.** Flat washes of color with darker pooled edges and paper grain. **Brush Size** is
  the size of the washes.
- **Stylize.** Three styles: **Relief** (lit from the **Light Angle**), **Ink Outline** (flat
  colors with black lines) and **Cutout** (flat colors). **Levels** is the number of colors per
  channel in the last two.

## Light

- **Aperture Diffraction.** Starburst spikes from the bright lights: **Spikes**, **Length**,
  **Threshold** (how bright a light must be), **Intensity**, **Rotation** and a **Tint**.
- **Lens Reflections.** Faint colored reflections of the bright lights, mirrored through the lens
  center (move it with **Center X / Y**): **Reflections** is how many, **Spread** how far from
  the lights, **Softness** how out of focus.

## Color, Key and Transform

- **Color Palette** (Color). Finds the dominant colors of the picture and shows them as a strip
  of swatches at the **Position** you pick, biggest first.
- **3D Keyer** (Key). Keys out up to three picked colors. It compares colors in color space,
  so it catches all the shades of one screen color in one go. **Tolerance** and **Softness** set
  how far from the picked colors a color is still keyed, **Brightness Weight** how much
  brightness counts, **Clip Black** and **Clip White** tidy the matte, and **View** shows the
  matte. Use the eyedropper next to **Color 1** to pick from the viewer. See also [Remove a
  green screen](remove-a-green-screen.md).
- **Video Collage** (Transform). Shows the picture again in a grid of tiles: **Columns**,
  **Rows**, **Spacing**, **Corner Radius**, a **Shadow**, and how the picture fits each tile.
