# Color, Audio and Compositing page gaps

Goal for 1.10.0: a user can follow any YouTube guide for DaVinci Resolve's Color page, Fairlight (Audio) page and Fusion (Compositing) page and it just works. This file lists the most common beginner and intermediate tutorial workflows for each page and whether Kinoklip can do them today, step by step. Checked on 2026-11-17 against the code, [color-page.md](color-page.md), [behavior-spec.md](behavior-spec.md) (sections 37, 44), [effects-library-status.md](effects-library-status.md) and [roadmap.md](roadmap.md).

Clean room ([clean-room.md](clean-room.md)): the names below are those of Resolve's public reference manual and of public tutorials; nothing was taken from Resolve itself.

- **works**: the tutorial can be followed with the same palette, button or menu names.
- **partly**: the main steps work; the named piece is missing.
- **missing**: a tutorial step fails at the named element.

Every missing piece has a line in [roadmap.md](roadmap.md) under "Next version (1.10.0)".

## Color page

| # | Workflow | Status | What is missing |
|---|---|---|---|
| C1 | Blur or censor a face with a tracked window | works | Blur palette (spec 106) and Tracker palette (spec 107: Track Forward / Reverse, Pan / Tilt / Zoom / Rotate / 3D, Clip / Frame, graph, Clear Track); not done: Interactive mode, Cloud tracker |
| C2 | Blur a polygon or circle window (static) | works | |
| C3 | Sharpen part of the picture (Blur palette, Sharpen mode) | works | |
| C4 | Mist / soften skin or glow with the Mist mode | works | |
| C5 | Power windows: Circle, Linear, Polygon, Gradient (softness, invert, opacity, keyframes) | works | |
| C6 | Curve (Bezier) power window | works | Curve button in the Windows palette; Bezier points and tangents on the viewer, smooth / corner, point insertion (spec 111) |
| C7 | Window follows the subject (animate or track a window) | works | Tracker palette moves, scales and turns every window of the node (spec 107); polygon and curve shapes can be keyframed (spec 111) |
| C8 | Tracker and Stabilizer palettes on the Color page | works | The Tracker palette has Window and Stabilizer modes (spec 107); a node follows a track through its windows. The Stabilizer mode sets the clip's Stabilization (Mode, Zoom, Smooth, Strength, Stabilize); the Interactive mode, a tracked node without a window, and the Cloud tracker are not done |
| C9 | Qualifier HSL: pick colours, adjust ranges and softness, invert, Highlight | works | |
| C10 | Qualifier clean-up: Matte Finesse (Clean Black / White, Blur Radius, Shrink / Grow, In / Out Ratio, Denoise) | works | Matte Finesse section of the Qualifier palette (spec 108); Highlight shows the cleaned matte; not done: Black Clip / White Clip |
| C11 | Qualifier 3D and Luma modes, RGB mode | works | HSL / RGB / Luma / 3D mode menu of the Qualifier palette; the eyedroppers pick colours in the active mode (spec 108); the 3D selection is a soft sphere around each picked colour, not a hand-shaped volume |
| C12 | Skin tone with a qualifier and a Hue vs Sat curve | partly | works (Matte Finesse, spec 108); the vectorscope skin tone line exists |
| C13 | Change the sky colour (qualifier plus gradient window) | works | |
| C14 | Sky replacement | missing | Sky Replacement effect (planned in effects-library-status.md) |
| C15 | Primaries wheels: Lift, Gamma, Gain, Offset, Contrast, Pivot, Sat, Hue, Lum Mix, Temp, Tint | works | |
| C16 | Primaries extras: Shad, Hi, Color Boost, Midtone Detail, Auto Color / Auto Balance buttons | works | Boost, Shad, Hi, MD and Auto in the Primaries palette (spec 146) |
| C17 | Primaries bars mode | works | |
| C18 | Log wheels (Shadow, Midtone, Highlight, Offset with Low / High range) | works | Log palette: Shadow, Midtone, Highlight, Offset wheels, Low / High Range (spec 146) |
| C19 | Custom curves Y, R, G, B with the Link toggle | works | Soft Clip fields and the Pick eyedropper (spec 146) |
| C20 | Hue vs Hue, Hue vs Sat, Hue vs Lum, Lum vs Sat, Sat vs Sat | works | Sat vs Lum curve and the Pick eyedropper adds the three points (spec 146) |
| C21 | Color Warper (Hue - Sat grid, Chroma - Luma grid) | missing | Color Warper palette |
| C22 | HDR palette (zones, Global, Shadow, Light, Highlight, Specular) | missing | HDR palette |
| C23 | RGB Mixer | works | Spec 109: Red / Green / Blue output tabs with three sliders each, Monochrome, Preserve Luminance; not done: HSV / HSL mixer spaces |
| C24 | Motion Effects: Temporal and Spatial Noise Reduction, Motion Estimation | partly | Edit page Noise Reduction effect (spatial); no Motion Effects palette, no temporal noise reduction, no Better / Faster modes |
| C25 | Magic Mask (draw a stroke on a person or object, it follows) | missing | Magic Mask palette; the optional Background Removal model only cuts a person out on the Edit page |
| C26 | Color match / Shot Match to another clip | works | Color Match palette (spec 153): Shot Match to This Clip (strip right-click) and the ColorChecker Classic chart match; no chart auto-detection; Color Stabilizer is another effect |
| C27 | Apply a LUT to a node, a clip or the timeline (3D LUT browser) | works | |
| C28 | Export a LUT from a grade (Generate 3D LUT) | works | Generate 3D LUT (CUBE) in the clips strip menu (spec 146) |
| C29 | Node types: Serial, Parallel, Layer | works | |
| C30 | Node types: Outside, Key Mixer, Splitter / Combiner, compound node, typed node labels | missing | Add Outside Node (Alt+O), Add Key Mixer, Splitter / Combiner, Compound Node, Node Label field |
| C31 | Alpha output and key: node Key tab, Key Output, Alpha Output | partly | Key tab has gain, invert and matte; no Alpha Output node, no key input / output ports, a grade cannot make the clip transparent |
| C32 | Gallery stills: Grab Still, apply, wipe, rename, delete | works | |
| C33 | Gallery albums, Power Grade albums, Memories, import and export stills (DRX), export a still as an image | works | Album list, PowerGrade albums shared by all projects, Memories A to Z (Alt+1..8 save, Ctrl+1..8 apply), Append Node Graph, Import / Export Still with a `.kkgrade` file (not Resolve's `.drx`) |
| C34 | Versions of a grade (Local / Remote, Create New Version) | missing | Versions menu on the clip |
| C35 | Groups with Pre-Clip and Post-Clip grades; Timeline grade | partly | Timeline grade works; Groups, Pre-Clip and Post-Clip grades, ColorTrace missing |
| C36 | Scopes: Waveform, RGB Waveform, Parade, Vectorscope, Histogram | works | |
| C37 | CIE chromaticity scope, scope gain / settings, HDR scales | works | CIE Chromaticity scope and scope settings Scale and Gain (spec 146); HDR scales not done |
| C38 | Color space transform, ACES, color managed project, input colour space per clip | missing | Color Space Transform, ACES Transform, Gamut Mapping and Chromatic Adaptation effects (planned in effects-library-status.md); Color Management in Project Settings; Input Color Space in Clip Attributes |
| C39 | Film grain and film look (Film Grain, Halation, Vignette, Film Look) | partly | The Edit page effects exist; no OpenFX / Resolve FX palette to apply them to a node on the Color page |
| C40 | Face Refinement, Depth Map, Relight (Neural Engine tools) | missing | The three effects (planned) with optional local open models |
| C41 | Lens Blur, Glow and other effects on one node, limited by its window or qualifier | partly | Effects exist on the Edit page; a node cannot hold a Resolve FX, so a window or qualifier cannot limit them (OpenFX palette) |
| C42 | Vignette with an inverted circle window and Gain | works | |
| C43 | Copy a grade to other clips (Ctrl+C / Ctrl+V, middle-click, Apply Grade) | works | |
| C44 | Dynamic grade with keyframes | partly | Add / Remove Keyframe works; no Keyframes palette / strip on the Color page, no Dynamic interpolation |
| C45 | Sizing palette (Zoom, Pan, Tilt, Rotate, Flip, Input / Output sizing) | partly | Edit page Transform does it; no Sizing palette on the Color page |
| C46 | Compare shots: Gallery wipe, Split Screen, Lightbox, Highlight | works | Split Screen (Selected Clips, Timeline), Lightbox (spec 44b.32, 44b.33) |
| C47 | Timeline grade and Adjustment clips | works | |
| C48 | Output blanking / letterbox bars | works | Timeline > Output Blanking presets (spec 44b.34) |

## Audio page (Fairlight)

| # | Workflow | Status | What is missing |
|---|---|---|---|
| A1 | Per-track EQ (6 bands) with the curve editor | works | |
| A2 | EQ presets (voice, music, telephone) | works | Presets menu in the EQ window: seven factory presets and your own, one undo step (spec 137) |
| A3 | High-pass filter against rumble | works | |
| A4 | Compressor on a voice track | works | |
| A5 | Limiter on the master | works | |
| A6 | Noise gate and expander | missing | Gate and Expander sections in the Dynamics window (spec 37.20) |
| A7 | De-esser | missing | De-Esser effect (Frequency, Range, Reduction) |
| A8 | Duck music under a voice (sidechain compressor) | partly | Level lines and fader automation work by hand; Dynamics has no Sidechain input / Key listen, and no Ducking helper |
| A9 | Voice Isolation | missing | Voice Isolation button on the track and in the Inspector (local open model) |
| A10 | Noise Reduction (Fairlight FX, Learn noise print) | works | Noise Reduction audio effect on a clip: Auto Speech (RNNoise, optional AI) or Manual Learn (spectral gate with a learned profile), Threshold, Attack, Sensitivity, Ratio, Reduction, Dry / Wet, Output Gain (spec 132); also as a mixer track slot (spec 133), no DeepFilterNet |
| A11 | Dialogue Leveler / Dialogue Processor | missing | Dialogue Leveler and Dialogue Processor effects (spec 37.28) |
| A12 | Normalize Audio Levels on clips (Sample Peak, True Peak, ITU-R BS.1770) | works | |
| A13 | Normalize a whole track, bus or the main mix to a loudness target | works | Normalize Audio Levels... on a mixer strip, a track header and the main bus: loudness, true peak or sample peak, one fader gain, one undo step (spec 134) |
| A14 | Loudness meter in LUFS (momentary, short-term, integrated, true peak) | works | Standard list (EBU R128, ATSC A/85, YouTube -14, Podcast -16, Custom), Loudness Range, over / under read-out, history graph, Start / Pause / Reset (spec 134) |
| A15 | Loudness normalisation when rendering | works | Deliver > Audio > Normalize audio to a standard, true peak limited (spec 134) |
| A16 | Buses: main bus and submix buses | works | Bus-to-bus feeds with Sends, Aux buses (send only), loops refused; no Main / Submix / Aux type menu (right-click a bus: Aux Bus) (spec 137) |
| A17 | Aux sends (reverb or delay send with a level knob) | works | Sends window on every track and bus strip: level, Pre / Post fader, pan, on / off (spec 137) |
| A18 | Effects Library with track effect slots (insert effects in the mixer) | works | Audio page: Effects Library (Audio FX) and Inspector panels, a + FX button, drop target and slots (bypass, remove, controls in the Inspector) on every track strip, saved with the project (spec 133); no slots on buses yet |
| A19 | Clip audio effects in the Inspector (Gain, Equalizer, Compressor, Limiter, filters, Delay, Reverb) | works | |
| A20 | Reverb with a room preset | works | Pre-delay, Decay, Room Size, Damping, Early Reflections, Width, Mix and six presets, as a clip effect and a track slot; the send is the Sends window (spec 137, 138) |
| A21 | Echo / Delay | works | |
| A22 | Pitch change of a clip | works | |
| A23 | Chorus, Flanger, Distortion, Stereo Width | works | Clip effects and track slots (spec 138) |
| A24 | Fader automation: Read, Touch, Write | partly | Latch and Trim modes; automating pan or plug-in parameters from the strip's Auto buttons works for fader and pan only |
| A25 | Edit automation curves on the timeline (automation lane) | missing | Automation lane under the track with Volume / Pan / effect parameter curves (spec 37.22) |
| A26 | Pan, including surround panner | partly | Stereo balance works, also on buses (spec 137); no Surround panner / 3D joystick |
| A27 | Fade handles and crossfades | works | |
| A28 | Clip volume with a level line and keyframes | works | |
| A29 | Record a voice-over on an armed track | works | |
| A30 | ADR (cue list, line-by-line recording) | missing | ADR panel (spec 37.24) |
| A31 | Mute, Solo, track mono / stereo format | works | |
| A32 | 5.1, 7.1 and adaptive track formats | missing | Track format choice beyond Mono / Stereo (spec 37.26) |
| A33 | Invert phase (polarity) on a track | works | Phase Invert button on every strip, right-click for Left or Right (spec 137) |
| A34 | Choose which source channel plays (Clip Attributes > Audio) | works | |
| A35 | Sync audio by waveform (dual-system sound) | works | Multicam sync by audio exists; also in the Media Pool |
| A36 | Bounce Mix to Track / Bounce Selected Tracks | missing | Both Timeline menu commands (roadmap item exists in Later) |
| A37 | Mixer track groups and VCAs (link faders) | missing | Track groups and VCA faders (spec 37.31) |
| A38 | Spectrum analyzer, phase / correlation meter | missing | Spectrum and Phase meters in the monitoring panel |
| A39 | Monitor output control (Dim, Mono check, speaker select) | partly | Output device in Preferences; no Dim / Mono / Mute monitor controls on the Audio page |
| A40 | Audio scrubbing and waveform view | works | |
| A41 | Dialogue / sound library search | missing | Sound Library panel (spec 37.27) |
| A42 | Elastic Wave (stretch audio to a clip's speed without moving pitch) | partly | Speed change with Pitch Correction works; Elastic Wave editing inside a clip is missing (spec 37.29) |
| A43 | Plug-ins (VST / AU / LV2 third-party effects) | missing | Plug-in host (spec 37.25) |

## Compositing page (Fusion)

The names are those of the Fusion reference manual and public tutorials. Status is checked against the Compositing page ([compositing-page.md](compositing-page.md), [behavior-spec.md](behavior-spec.md) sections 42, 88 to 94, [effects-library-status.md](effects-library-status.md)).

| # | Workflow | Status | What is missing |
|---|---|---|---|
| F1 | Text+ title with font, size, colour, outline and shadow | partly | Text node has those; no Text+ layout controls (Layout type Point / Frame / Circle / Path, Tracking, Line Spacing, Kerning), no Shading tab with several elements, no Rich Text |
| F2 | Text+ animated title with Follower (per-character animation) | missing | Text+ Follower modifier and per-character Transform / Shading; the Edit page Write On range is not in the Fusion Text node |
| F3 | Text+ Write On (Start / End keyframes) | partly | Write On exists on Edit page titles (spec 84); the Compositing Text node has no Write On range |
| F4 | Lower third (Text + Background + Merge, animate with Transform) | works |  |
| F5 | Merge basics: Foreground, Background, Blend, Apply Mode, Size, Center, Angle | works |  |
| F6 | Background node: solid colour, gradient, with a Merge | works |  |
| F7 | Transform: Center, Pivot, Size, Angle, Flip, Edges, Keyframes | works |  |
| F8 | Rectangle / Ellipse / Polygon mask, plug into a tool's Effect Mask | works |  |
| F9 | BSpline mask and rotoscoping a subject by keyframing points | partly | B-Spline and Polygon exist with shape keyframes; no Bezier mask node with tangent handles, no Roto assist / Ctrl+drag to add points, no Point Follower, no Solid / Open spline toggle |
| F10 | Mask tracking (a mask follows the subject) | missing | Tracker or Planar Tracker output cannot drive a mask's Center from a Mask Tracker; Connect to Tracker copies keys for Center only, not for polygon points, size or angle (roadmap: mask tracking on the Edit page) |
| F11 | Planar Tracker: screen replacement (Corner Pin output, Merge) | works | Planar Tracker has Corner Pin, Steady, Unsteady; Create Planar Transform is not needed (the Output modes cover it) |
| F12 | Planar Tracker with a mask excluding occluding objects (Track Mask) | missing | Planar Tracker has no Mask input to exclude areas, no Pattern / Occlusion mask |
| F13 | Corner Positioner / Corner Pin by hand | works |  |
| F14 | Tracker: stick text or an image to a moving object (Match Move) | partly | Tracker (point) works; one point per Tracker node (no several trackers in one node, so no Rotation / Scale from two patterns); Follow Track copies Center only |
| F15 | Stabilize a shaky shot with the Tracker (Stabilize) | works |  |
| F16 | Delta Keyer green screen: pick colour, Gain, Balance, Clean Background | partly | Delta Keyer exists; missing the advanced tabs: Pre-Matte Blur, Erode / Dilate, Matte Blur, Fringe, Replace Mode (spec 92.4) |
| F17 | Ultra Keyer green screen (Pre-Matte, Matte, Spill, Color Correction tabs) | missing | Ultra Keyer node |
| F18 | Clean plate for keying | partly | Clean Background / Foreground inputs in Delta Keyer; a clean plate is not a node input, and Clean Plate tool is missing |
| F19 | Chroma Keyer and Luma Keyer | works | Luma Keyer node, Chroma Key and 3D Keyer effect nodes |
| F20 | Matte control: Erode / Dilate, Blur, Matte Finesse on the alpha | partly | Alpha Matte Shrink and Grow effect; no Matte Control node (Garbage mask, Solid mask, Erode / Dilate, Blur, Restore Fringe) |
| F21 | Light Wrap on a keyed subject | missing | Light Wrap (Blur and Merge recipe works by hand; no ready node) |
| F22 | Glow and Soft Glow | partly | Glow effect works; no Soft Glow node (Threshold, Gain, Blur, Color Scale) |
| F23 | Blur, Directional Blur, Radial and Zoom Blur | works |  |
| F24 | Fast Noise: smoke, fog, clouds (animated Seethe) | partly | Fast Noise exists; Seethe, Seethe Rate and the Color gradient tab are not confirmed present and need a check |
| F25 | Particles: pEmitter, pRender, pMerge | missing | pEmitter, pRender, pMerge, pGravity, pTurbulence (roadmap Later: particles) |
| F26 | 3D: Text3D, Shape3D, Merge3D, Renderer3D | missing | Text3D, Shape3D, Image Plane 3D, Merge3D, Renderer3D, 3D viewer (roadmap Later: 3D) |
| F27 | 3D: Camera3D, lights and camera moves | missing | Camera3D, Spot / Point / Directional Light, Ambient Light |
| F28 | Paint tool: clone, erase, wire removal | missing | Paint node with Stroke, Clone, Eraser, Smear, and stroke Duration (roadmap Later: Paint) |
| F29 | Magic Mask in Fusion (and Surface Tracker / Object Removal) | missing | Magic Mask node, Object Removal, AI mask; optional local open model |
| F30 | Keyframes and the Spline editor (Bezier, Linear, Ease, Loop) | works | Spline and Keyframes panels, loops, Ping-Pong, Relative |
| F31 | Modifiers: Perturb, Wiggle, Offset, Expression, Cycle / Time | missing | Modifiers menu on a parameter (right-click > Modify With), Perturb, Offset, Expression, Calculation, XY Path; no parameter links either |
| F32 | Connect a parameter to another (Connect To, Publish, expressions such as `Transform1.Angle*2`) | missing | Parameter linking and the = expression field |
| F33 | Macros: group nodes, Make Macro, publish controls, save as a Fusion title | missing | Group, Macro Editor, Effects Library > Titles entry from a saved macro |
| F34 | Groups, Underlay and Sticky Notes in the node editor | partly | Notes with group frames exist (CompNote); no Group node that folds nodes into one tile, no Underlay that moves its nodes |
| F35 | Grid Warp and mesh warp | missing | Grid Warp node (Warper and Dent effects exist, but not a draggable grid) |
| F36 | Displace and Distort by a map | missing | Displace node (an image drives a shift), Vector Distort |
| F37 | Transitions made in Fusion (a Fusion transition and a MediaIn1 / MediaIn2 composite) | partly | Fusion Transitions exist as ready transitions (effects-library-status); a user cannot build one: the node graph has only one MediaIn and no transition mode with MediaIn2 and a Background mix |
| F38 | Loader and Saver (import a still or sequence, write a render) | partly | MediaIn takes any media pool item; no Loader (file path) node, no Saver node; render happens on the Deliver page |
| F39 | MediaIn and MediaOut with several MediaIns of the Media Pool | works |  |
| F40 | Instance tool (copy a node linked to the original) | missing | Instance node / Paste Instance |
| F41 | Background Removal / Depth Map in the Compositing page | partly | Edit page Background Removal effect appears as a node; no Depth Map, no Brush to refine, no Magic Mask |
| F42 | Drop Shadow and Shadow | works | Drop Shadow effect and Text node shadow |
| F43 | Vector Motion Blur and Motion Blur | partly | Motion Blur on Merge and Transform (spec 94) and the Motion Blur effect; no Vector Motion Blur node (uses motion vectors from the Optical Flow) |
| F44 | Time Speed and Time Stretcher | missing | Time Speed and Time Stretcher nodes on the page (Edit page speed ramps exist) |
| F45 | Color Corrector in Fusion (Color Correct tools: Gamma, Gain, Lift, Hue, Saturation, Color Curves) | partly | Color Corrector node (Color Balance, Levels, Saturation); no Color Curves, Hue Curves, Color Gain tools, no Brightness Contrast node, no Gamut node |
| F46 | Channel Booleans, Channel Swap, Set Alpha, Alpha Multiply / Divide | partly | Channel Booleans exists; no Alpha Multiply / Alpha Divide, Matte Control, Set Canvas Color or Channel Mapper as named tools |
| F47 | Custom Tool (pixel code) | missing | Custom Tool with expression fields (r1, r2, n1 ..., Setup / Intermediate tabs) |
| F48 | Viewer: A / B buffers, split, channel keys, checker, Lock, 100% | works | spec 90 |
| F49 | Viewer: onion skin, Region of Interest (RoD), Smart Control, Snap, Fit, 3D / Rulers | partly | Zoom and Fit exist; missing Onion Skin, ROI, Rulers, Show Safe Area, DoD overlay, Stereo and 3D view options |
| F50 | Inspector: Common Controls, Tools / Modifiers / Settings tab names | works |  |
| F51 | Node editor basics: Shift+Space, add after selection, insert on link, replace, find | works |  |
| F52 | Render cache and Fusion render settings (Proxy, Auto Proxy, Motion Blur toggle, High Quality) | partly | Viewer quality via Playback; missing Proxy / Auto Proxy / HiQ / MB toggle buttons on the page and cache-to-disk |
