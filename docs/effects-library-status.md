# Effects library status

151 of 183 entries work.

Every entry of the Effects library that [DaVinci Resolve](https://www.blackmagicdesign.com/products/davinciresolve) is known to have from its public documentation (names only), and whether Kinoklip has it. `works` means an effect, transition, generator, title or clip of the same job is registered in Kinoklip (sometimes under its own name or with other settings); `planned` means it is not there yet. Nothing here is marked `not planned`. Kinoklip's own extras that Resolve lacks are not listed. The research behind the list is the gap list for version 1.2; the behaviour of what landed is in [behavior-spec.md](behavior-spec.md) sections 54, 56 and 57. [scripts/status.py](../scripts/status.py) counts this table for the picture in the readme; run it after changing a row.

## Video transitions

| Name | Group | Kind | Status |
|---|---|---|---|
| Additive Dissolve | Dissolve | Video transition | works |
| Blur Dissolve | Dissolve | Video transition | works |
| Cross Dissolve | Dissolve | Video transition | works |
| Dip to Color Dissolve | Dissolve | Video transition | works |
| Non-Additive Dissolve | Dissolve | Video transition | works |
| Smooth Cut | Dissolve | Video transition | works |
| Arrow Iris | Iris | Video transition | works |
| Cross Iris | Iris | Video transition | works |
| Diamond Iris | Iris | Video transition | works |
| Eye Iris | Iris | Video transition | works |
| Hexagon Iris | Iris | Video transition | works |
| Oval Iris | Iris | Video transition | works |
| Pentagon Iris | Iris | Video transition | works |
| Square Iris | Iris | Video transition | works |
| Triangle Iris | Iris | Video transition | works |
| Barn Door | Motion | Video transition | works |
| Push | Motion | Video transition | works |
| Slide | Motion | Video transition | works |
| Split | Motion | Video transition | works |
| Box | Shape | Video transition | works |
| Heart | Shape | Video transition | works |
| Star | Shape | Video transition | works |
| Triangle Left | Shape | Video transition | works |
| Triangle Right | Shape | Video transition | works |
| Band Wipe | Wipe | Video transition | works |
| Center Wipe | Wipe | Video transition | works |
| Clock Wipe | Wipe | Video transition | works |
| Edge Wipe | Wipe | Video transition | works |
| Radial Wipe | Wipe | Video transition | works |
| Spiral Wipe | Wipe | Video transition | works |
| Venetian Blind Wipe | Wipe | Video transition | works |
| X Wipe | Wipe | Video transition | works |
| Brightness Flash | Fusion Transitions | Video transition | works |
| Camera Shake | Fusion Transitions | Video transition | works |
| Circles | Fusion Transitions | Video transition | works |
| Crash Zoom | Fusion Transitions | Video transition | works |
| Cross Dissolve (Fusion) | Fusion Transitions | Video transition | works |
| Drop Warp | Fusion Transitions | Video transition | works |
| Fall and Bounce | Fusion Transitions | Video transition | works |
| Film Strip | Fusion Transitions | Video transition | works |
| Flip 3D | Fusion Transitions | Video transition | works |
| Foreground Wipe | Fusion Transitions | Video transition | works |
| Glitch | Fusion Transitions | Video transition | works |
| Noise Dissolve | Fusion Transitions | Video transition | works |
| Paint On | Fusion Transitions | Video transition | works |
| Pan Down | Fusion Transitions | Video transition | works |
| Pan Left | Fusion Transitions | Video transition | works |
| Pan Right | Fusion Transitions | Video transition | works |
| Pan Up | Fusion Transitions | Video transition | works |
| Rotate | Fusion Transitions | Video transition | works |
| Rotate 90 | Fusion Transitions | Video transition | works |
| Round and Down | Fusion Transitions | Video transition | works |
| Slice Push | Fusion Transitions | Video transition | works |
| Slide Down | Fusion Transitions | Video transition | works |
| Slide Left | Fusion Transitions | Video transition | works |
| Slide Right | Fusion Transitions | Video transition | works |
| Slide Up | Fusion Transitions | Video transition | works |
| Tunnel of Light | Fusion Transitions | Video transition | works |
| Warp | Fusion Transitions | Video transition | works |
| Zoom In | Fusion Transitions | Video transition | works |
| Zoom In and Out | Fusion Transitions | Video transition | works |
| Burn Away | Resolve FX Transitions | Video transition | works |
| DCTL Transition | Resolve FX Transitions | Video transition | planned |

## Audio transitions

| Name | Group | Kind | Status |
|---|---|---|---|
| Cross Fade +3 dB | Cross Fade | Audio transition | works |
| Cross Fade 0 dB | Cross Fade | Audio transition | works |
| Cross Fade -3 dB | Cross Fade | Audio transition | works |

## Generators

| Name | Group | Kind | Status |
|---|---|---|---|
| 10 Step | Generators | Generator | works |
| 100mV Steps | Generators | Generator | works |
| BT.2111 Color Bar HLG Narrow | Generators | Generator | works |
| BT.2111 Color Bar PQ Full | Generators | Generator | works |
| BT.2111 Color Bar PQ Narrow | Generators | Generator | works |
| EBU Color Bar | Generators | Generator | works |
| Four Color Gradient | Generators | Generator | works |
| Grey Scale | Generators | Generator | works |
| Noise Gradient | Generators | Generator | works |
| SMPTE Color Bar | Generators | Generator | works |
| Solid Color | Generators | Generator | works |
| Window | Generators | Generator | works |
| YCbCr Ramp | Generators | Generator | works |

## Titles

| Name | Group | Kind | Status |
|---|---|---|---|
| Text | Titles | Title | works |
| Text+ | Titles | Title | planned |
| Scroll | Titles | Title | works |
| Left Lower Third | Titles | Title | works |
| Middle Lower Third | Titles | Title | works |
| Right Lower Third | Titles | Title | works |

## Effects section

| Name | Group | Kind | Status |
|---|---|---|---|
| Adjustment Clip | Effects section | Effects section entry | works |
| Fusion Composition | Effects section | Effects section entry | works |
| Binoculars | Effects section | Effects section entry | works |
| CCTV | Effects section | Effects section entry | planned |
| Colored Border | Effects section | Effects section entry | works |
| Digital Glitch | Effects section | Effects section entry | planned |
| Drone Overlay | Effects section | Effects section entry | planned |
| DSLR | Effects section | Effects section entry | planned |
| DVE | Effects section | Effects section entry | planned |
| Night Vision | Effects section | Effects section entry | works |
| Video Call | Effects section | Effects section entry | planned |
| Video Camera | Effects section | Effects section entry | planned |

## Video effects

| Name | Group | Kind | Status |
|---|---|---|---|
| Box Blur | Blur | Video effect | works |
| Directional Blur | Blur | Video effect | works |
| Gaussian Blur | Blur | Video effect | works |
| Lens Blur | Blur | Video effect | works |
| Mosaic Blur | Blur | Video effect | works |
| Radial Blur | Blur | Video effect | works |
| Zoom Blur | Blur | Video effect | works |
| ACES Transform | Color | Video effect | planned |
| Chromatic Adaptation | Color | Video effect | planned |
| Color Compressor | Color | Video effect | planned |
| Color Space Transform | Color | Video effect | planned |
| Color Stabilizer | Color | Video effect | planned |
| Contrast Pop | Color | Video effect | works |
| DCTL | Color | Video effect | planned |
| Dehaze | Color | Video effect | works |
| Despill | Color | Video effect | works |
| False Color | Color | Video effect | works |
| Flicker Addition | Color | Video effect | works |
| Gamut Limiter | Color | Video effect | works |
| Gamut Mapping | Color | Video effect | planned |
| Invert Color | Color | Video effect | works |
| Color Generator | Generate | Video effect | works |
| Color Palette | Generate | Video effect | works |
| Grid | Generate | Video effect | works |
| 3D Keyer | Key | Video effect | works |
| Alpha Matte Shrink and Grow | Key | Video effect | works |
| HSL Keyer | Key | Video effect | works |
| Luma Keyer | Key | Video effect | works |
| Aperture Diffraction | Light | Video effect | works |
| Glow | Light | Video effect | works |
| Halation | Light | Video effect | works |
| Lens Flare | Light | Video effect | works |
| Lens Reflections | Light | Video effect | works |
| Light Rays | Light | Video effect | works |
| Beauty | Refine | Video effect | works |
| Depth Map | Refine | Video effect | planned |
| Face Refinement | Refine | Video effect | planned |
| Relight | Refine | Video effect | planned |
| Automatic Dirt Removal | Revival | Video effect | planned |
| Chromatic Aberration | Revival | Video effect | works |
| Dead Pixel Fixer | Revival | Video effect | works |
| Deband | Revival | Video effect | works |
| Deflicker | Revival | Video effect | planned |
| Dust Buster | Revival | Video effect | planned |
| Frame Replacer | Revival | Video effect | planned |
| Noise Reduction | Revival | Video effect | works |
| Object Removal | Revival | Video effect | planned |
| Patch Replacer | Revival | Video effect | planned |
| Sharpen | Sharpen | Video effect | works |
| Sharpen Edges | Sharpen | Video effect | works |
| Soften and Sharpen | Sharpen | Video effect | works |
| Abstraction | Stylize | Video effect | works |
| Drop Shadow | Stylize | Video effect | works |
| Edge Detect | Stylize | Video effect | works |
| Emboss | Stylize | Video effect | works |
| Mirrors | Stylize | Video effect | works |
| Pencil Sketch | Stylize | Video effect | works |
| Prism Blur | Stylize | Video effect | works |
| Scanlines | Stylize | Video effect | works |
| Sky Replacement | Stylize | Video effect | planned |
| Stylize | Stylize | Video effect | works |
| Tilt-Shift Blur | Stylize | Video effect | works |
| Vignette | Stylize | Video effect | works |
| Watercolor | Stylize | Video effect | works |
| Motion Blur | Temporal | Video effect | planned |
| Motion Trails | Temporal | Video effect | planned |
| Smear | Temporal | Video effect | planned |
| Stop Motion | Temporal | Video effect | works |
| Analog Damage | Texture | Video effect | works |
| Detail Recovery | Texture | Video effect | works |
| Fast Noise | Texture | Video effect | works |
| Film Damage | Texture | Video effect | works |
| Film Grain | Texture | Video effect | works |
| JPEG Damage | Texture | Video effect | works |
| Texture Pop | Texture | Video effect | works |
| Camera Shake | Transform | Video effect | works |
| Match Move | Transform | Video effect | planned |
| Surface Tracker | Transform | Video effect | planned |
| Transform | Transform | Video effect | works |
| Video Collage | Transform | Video effect | works |
| Dent | Warp | Video effect | works |
| Lens Distortion | Warp | Video effect | works |
| Ripples | Warp | Video effect | works |
| Vortex | Warp | Video effect | works |
| Warper | Warp | Video effect | planned |
| Waviness | Warp | Video effect | works |
