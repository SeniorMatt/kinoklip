# Color page (design)

Owner request 2026-11-04: build the Color page's engine now, plus a minimal page that makes it
usable; a second agent then builds the full page (node editor, Resolve layout) on this API.
Clean room ([clean-room.md](clean-room.md)): behaviour from general knowledge of how a Resolve
colourist expects grading to work; our own maths, drawing, names and icons; never Resolve
itself. Spec: [behavior-spec.md](behavior-spec.md) section 44 (44a core, 44b page).

## What it is for the user
- Every clip on the timeline can have a **grade**: a small node graph of corrector nodes,
  in series (each node sees the previous one's result) or combined by **Parallel** and
  **Layer** mixers. The **timeline** has a grade of its own, applied after every clip grade.
- Open the Color page with the page bar or **Shift+6**. It grades the clip under the
  playhead on the highest enabled video track (move the playhead to grade another clip); the
  **Timeline Grade** button switches to the timeline's grade.
- A grade plays on the Edit page and renders on the Deliver page exactly as on the Color page
  (one evaluation path); **Shift+D** bypasses every grade in the viewers only (the render and
  the render cache always keep them).

## Where the grade sits in the pipeline
Per clip, in this order (GPU `GlRenderer::drawClip()`, CPU `CpuRenderer::drawClip()`):

1. **Source**: decode (with the clip's input interpretation: Clip Attributes, proxies) or the
   generated picture; or the clip's **composition** (Compositing page) when it has one.
2. **Edit page effects** (Inspector effects, in their order), including the Apply LUT effect.
   This is the clip's "input transform" stage in this release: a camera LUT is an Apply LUT
   effect, or a LUT node at the head of the grade.
3. **Clip grade** (Color page).
4. **Masks** of the clip (Edit page), then crop, transform, opacity, transitions and
   compositing of the tracks.
5. **Timeline grade** over the composite of every video track.
6. Captions (subtitle tracks), then the output (viewer, export, render cache).

Decisions:
- **Grade after the Edit page effects.** A Resolve user expects the Color page to see what the
  Inspector made of the clip (a stabilised, retimed, keyed clip), and expects the grade to stay
  with the clip's own picture: it moves, scales and fades with the clip (windows are in clip
  content coordinates), so the clip grade comes before the transform and the compositing.
- **Compositing page.** The composition replaces the clip's source picture (step 1), so the
  grade grades the composition's output, which matches Resolve's page order (compositing
  before colour). A composition's MediaIn of its own clip therefore shows the clip *ungraded*
  (there is nothing to fix up: the grade comes after the composition anyway). Compositing
  page viewers show composition nodes, ungraded.
- **Adjustment clips** carry grades like other clips: the grade then applies to everything below
  within the adjustment clip's footprint.
- **Captions are not graded** (the timeline grade comes before them).
- **Nested timelines** keep their own timeline grade (a compound clip looks like its timeline).

## Model (`src/core/Grade.h`)
- `ClipGrade { nodes, output }` — used for clips (`ClipData::grade`) and timelines
  (`TimelineData::grade`). Empty = ungraded (renders exactly as before, no pass at all).
- `GradeNode`: `id`, `type` (Corrector, Parallel, Layer), `label`, `enabled` (bypass), `params`
  (`gradekeys::*`, all keyframeable), `curves` (`GradeCurves`), `qualifier` (`GradeQualifier`),
  `windows` (`GradeWindow`), `lutPath` + `lutInterpolation`, `inputs` (node ids, 0 = the grade's
  input picture), `pos` (node editor position). Node and window ids share one id space per grade.
- **Primaries** (`gradekeys`): Lift / Gamma / Gain / Offset wheels with R, G, B and master Y,
  Contrast + Pivot, Saturation, Hue, Temperature, Tint, Lum Mix, and **Mix** (the node's
  opacity / key output gain).
- **Curves**: Custom Y, R, G, B; Hue vs Hue, Hue vs Sat, Hue vs Lum, Lum vs Sat, Sat vs Sat.
  Monotone cubic (no overshoot); hue curves wrap. Soft clip: later.
- **Qualifier**: HSL ranges with soft edges, per-channel enable, invert. Static (not keyed).
- **Power windows**: Circle, Linear (rectangle), Polygon, Gradient; centre, size, rotation,
  inside / outside softness and opacity keyframeable (`gradewindowkeys`), invert per window.
  Several windows combine as a union. Coordinates are fractions of the graded picture.
- **LUT node**: any corrector can load a `.cube` (1D or 3D), applied last in the node,
  tetrahedral (default) or trilinear.
- Midtone detail, soft clip, the HDR and log wheels, tracking of windows, shape keyframes of
  polygons, Bezier windows, key mixers and splitter / combiner nodes: later.

### Node semantics (identical on GPU and CPU)
- **Corrector**: input `I`, graded `G` = primaries -> curves -> LUT; key `k` = qualifier of `I` x
  window matte x Mix; output `O = I + (G - I) k`. A bypassed node outputs `I` (and has key 0
  under a Layer mixer).
- **Parallel mixer**: the equal average of its inputs' outputs (at most 8 inputs).
- **Layer mixer**: input 0 at the bottom; every further input `i` laid over the result `R` with
  its key: `R = (1 - k) R + k I_i + (O_i - I_i)` (= `mix(R, G_i, k)` without dividing by `k`).
  Inputs that are mixers or the grade input replace `R`. The last input is on top.
- The grade works on straight (un-premultiplied) gamma-encoded RGB, unclamped between nodes
  (curves and the qualifier clamp their inputs to 0..1); alpha passes through unchanged.

### Maths (`src/core/GradeMath.h`)
Order inside a corrector: offset (RGB + master), temperature / tint (channel gains),
per-channel lift / gamma / gain (`pow(max(gain (x + lift (1 - x)), 0), 2^(-2 gamma))`), master
lift / gamma / gain mixed between RGB and luma-only by Lum Mix, contrast about the pivot, hue
rotation about the grey axis, saturation about Rec. 709 luma; then the curves; then the LUT.
`render/GlGrade.cpp` mirrors every function in GLSL; `render/GradeEval.cpp` runs the graph per
pixel on the CPU (reference and CPU fallback).

## Operations and undo
- Pure graph operations in `gradeops::` (Grade.h): `initialize`, `addSerialNode` (Alt+S),
  `addSerialNodeBefore`, `addParallelNode` (Alt+P), `addLayerNode` (Alt+L), `removeNode`,
  `connect` / `disconnect` (cycles refused), `setNodeEnabled` (Ctrl+D), `resetNode`,
  `addWindow` / `removeWindow`, `evaluationOrder`, `isIdentity`, `defaultSelection`.
- One undoable entry point each: `Project::setClipGrade()`, `setClipGrades()` (several clips in
  one step: apply a still, copy a grade), `setTimelineGrade()`; read with `clipGrade()` /
  `timelineGrade()`. They follow the beginEdit / endEdit / snapshot protocol: inside an open
  edit (a wheel drag) every call joins that one step; an unchanged grade makes no step.
- Split / trim shift grade keys like every clip key; a through edit joins only when both
  halves carry the same grade.
- Keyframes: KeyframeOps paths `grade/<node id>/<key>` and `gradewin/<window id>/<key>`; the
  keyframe editors list a "Color 01" group per keyed node.
- **Gallery** (`core/GradeGallery.h`, `Project::gallery()`): stills = a small JPEG picture and
  the grade at grab time; saved with the project, not undoable (a still store), changes mark
  the project modified.

## Format (project format 16)
- Clips and timelines may carry `"grade"`; the project may carry `"gallery"`. Only non-default
  values are written; an ungraded project's file differs from format 15 only in its version.
- Files of versions 1 - 15 load unchanged (nothing graded, empty gallery); older builds refuse
  format 16 instead of silently dropping grades.
- Damaged data is repaired on load: duplicate or zero node ids dropped, unknown inputs read the
  grade input, window ids made unique.
- LUT paths are stored absolute (not yet relative to the project or packed into single-file
  projects; later, through FileParams).

## Rendering
- GPU: `render/GlGrade` (one pass per correcting node, a key pass for keyed nodes under a
  Layer mixer, one pass per mixer input; curve tables, LUTs and polygon points as small float
  textures read with `texelFetch`, so GPU and CPU interpolate identically). The clip grade
  forces the clip onto the layer path (layer at output resolution x zoom, capped at the
  source's); the timeline grade runs on the accumulator.
- CPU fallback: `render/GradeEval` per pixel on the 8-bit picture, multi-threaded by rows; the
  same pixels within 2 levels (tests compare both on every feature).
- Render cache: clip JSON (with the grade) is part of every chunk key, the timeline grade and
  the LUT files' size / time are added; graded clips count as "effects" for Smart caching.
  `RenderRequest::bypassGrades` skips the cache (its chunks are graded).
- `samePictures()` sees the timeline grade, so the viewer re-renders after a timeline grade
  change.

## Scopes (`src/render/Scopes.h`, `src/ui/scopes`)
- Waveform (luma), RGB waveform, RGB Parade, Vectorscope (Rec. 709 Cb / Cr, 75 % targets,
  skin tone line) and Histogram, computed on the CPU from a 480 px wide render of the program
  frame (graded, what the viewer shows): a few milliseconds per frame, no GPU readback of the
  full frame. A GPU path is not needed at this size.
- `ScopeView` (one scope), `ScopesPanel` (with its type menu), `ScopeFeeder` (renders the
  current frame small through `PlaybackEngine::renderImage()`, one in flight),
  `ScopesWindow` (View > Video Scopes, Ctrl+Shift+W: two scopes in a floating window on any
  page).

## Page (44b)
`src/app/pages/ColorPage` with `src/ui/color`. Thin: everything goes through `ColorSession`.
Owner request 2026-11-04 (second agent, branch colorui): the full Resolve-style page on the
engine's API, replacing the minimal page (node list, curve picker, gallery strip).

```
| Gallery | viewer: Clip / Timeline, clip name, Highlight, Wipe, Bypass   | Nodes |
|         | the picture + on-screen controls; transport, timecode         |       |
|-----------------------------------------------------------------------------------|
| clips strip: numbered thumbnails, timecode, track; the graded clip outlined     |
| mini timeline (video tracks, playhead)                                          |
|-----------------------------------------------------------------------------------|
| Primaries | Curves | Qualifier | Windows | LUTs        Reset | Scopes | Key      |
```

- **Interface bar**: Gallery, Clips, Timeline (left), Nodes, Scopes (right); each hides its
  panel (`PanelId::Gallery`, `Clips`, `ColorTimeline`, `Nodes`, `Scopes`). Layout, palette,
  bars mode and panel visibility are saved per page (layout version 2).
- **Viewer**: the program picture (graded); `ColorViewerOverlay` on top draws the window
  controls, the highlight picture and the wipe. The transport buttons are the registry's
  transport.* commands (they act on the program engine here).
- **Node editor** (`ColorNodeEditor`): corrector tiles with their number ("01", the label if
  set), markers (Q qualifier, W windows, LUT) and a thumbnail of the node's output; Parallel
  ("P") and Layer ("L") mixers as small tiles with one port per input plus a free one; the
  source and output ends left and right of the graph. Drag from an output (or the source) to
  an input, a tile or the output end to connect; pick up a connected input and drop it on
  another input to reconnect, into empty space to disconnect; drag the output end onto a node to
  make it the output; drag a tile to move it (`GradeNode::pos`). Every gesture is one undo
  step; middle-drag pans, the wheel zooms, a double-click on the background fits. Right-click:
  add serial / serial before / parallel / layer, enable / disable, reset node, delete, reset
  grade, fit.
- **Primaries**: Lift / Gamma / Gain / Offset as wheels (relative puck drag, master strip,
  Shift = fine, double-click resets the wheel or the strip) or as bars (Y R G B), the Y R G B
  fields under each, Temp / Tint / Contrast / Pivot above and Mix / Sat / Hue / Lum Mix
  below; drags of wheels, bars and fields are one undo step each.
- **Curves**: Custom (Y / R / G / B; Link on: an edit of R, G or B sets all three, so the
  colour channels move together while Y stays separate) and the five HSL curves; click adds,
  drag moves, right-click or double-click removes; the palette's Reset clears the shown curve.
- **Qualifier**: Pick / Pick + / Pick - eyedroppers (click or drag a rectangle over the viewer;
  the colours come from the node's input picture through `grademath::rgbToHsl()`,
  `ui/color/QualifierPick`), the Hue / Sat / Lum bars (drag the range or its ends) and fields,
  Qualify / Invert / per-channel enables; Highlight (Shift+H) shows the node's key (qualifier
  x windows x Mix) over a flat grey or as a black and white matte.
- **Windows**: Circle / Linear / Polygon / Gradient buttons add windows to the current node
  (several per node), the list (enable check boxes), Invert, Delete and the fields. In the
  viewer (while the Windows palette is shown): drag inside to move, side handles for width /
  height, the corner for both, the knob for rotation (Shift: 15 degree steps), the left diamond
  for the outside softness; polygon points drag, a double-click on an edge inserts one, a
  right-click deletes one (three stay).
- **LUTs**: a browser of the .cube files in the LUT folders (the user's LUT folder in the
  application data plus "Add Folder..."), double-click applies to the current node, "Load
  File...", Remove, interpolation.
- **Gallery**: Grab Still (title bar, Ctrl+Alt+G); double-click or middle-click applies a still
  to the graded clip; its menu applies it to the clips selected in the strip, wipes, renames,
  deletes. **Wipe** (Ctrl+W, the header's Wipe): the still left of a vertical line, drag the
  line; the selected still, else the newest.
- **Clips strip**: every video clip of the timeline in order (start, then track). Click grades a
  clip (the playhead moves to its first frame when it is elsewhere; a lower-track clip is
  pinned); Ctrl / Shift click select several; middle-click copies that clip's grade to the
  selected clips (or the graded one); Ctrl+C / Ctrl+V copy the graded clip's grade and paste it
  to the selected clips. The mini timeline below moves the playhead.
- **Scopes / Key** (bottom right): the scope with its type menu; the Key tab shows the node's
  key output gain (Mix), Invert and a small matte.
- **Keys** (scope `widget:color_page`, claimed by the page through ShortcutOverride except while
  a text field has focus): Alt+S, Shift+S, Alt+P, Alt+L, Delete / Backspace, Ctrl+D, Shift+D,
  Shift+Home (reset node), Ctrl+Home (reset grade), Ctrl+Alt+G, Ctrl+C, Ctrl+V, Shift+H, Ctrl+W.
  The Edit page's timeline commands are suspended while the page is shown.

Decisions (colorui):
- **Own node editor**, not the Compositing page's: a grade is a different graph (numbered
  correctors with one input, mixers with an input list, no named ports, notes or node
  library) and that editor is built on CompSession / CompAdapter; sharing it would mean an
  abstraction layer over both models inside a page another agent is changing. The conventions
  are the same (custom painting from the value, middle-drag pan, wheel zoom, cubic links).
- **Pictures beside the viewer** come from `ColorFrameSource`: the ungraded program frame at the
  playhead (960 px wide, `PlaybackEngine::renderImage(..., withGrades = false)`), graded on the
  CPU with `render::applyGrade()` for node thumbnails, the highlight and the eyedropper. The
  frame is the program composite, so these are exact for a clip that covers the frame
  untransformed; the window overlay itself follows the clip's transform
  (`render::clipTransform()`).
- **Clip thumbnails** are the graded program frame at each clip's middle frame (112 x 63), so a
  clip covered by a higher track shows the higher one there; they refresh when the clip, its
  grade or the timeline grade change.
- **Default node selection**: when the grade's output is a mixer the page starts on the last
  corrector (the palettes edit correctors).
- **Key / Sizing**: only a small Key tab (gain, invert, matte) beside the scopes; Sizing is
  the Edit page's transform for now.
- **Keys** not confirmed for Resolve's Windows defaults are marked "unsure" in the keymap
  (Shift+Home, Ctrl+Home, Ctrl+W); Shift+S (serial before) shares its key with the Edit page's
  audio scrubbing toggle, which the Color page claims while it has focus.

## API for the full page (second agent)
- Model and maths: `core/Grade.h`, `core/GradeMath.h` (evaluateCurve / addCurvePoint for curve
  editors, rgbToHsl for qualifier pickers, evaluateWindows / windowCoverage for window
  overlays), `core/GradeGallery.h`.
- Session: `ui/color/ColorSession.h` — target (clip / timeline), clip at playhead, selected node,
  `commit()`, node commands, `setParam(s)` / `setCurve` at the playhead, `beginGesture()` /
  `endGesture()`, bypass all, grab / apply still, copy grade. A node editor needs only
  `gradeops::connect / disconnect / removeNode` + `commit()`; GradeNode::pos is stored.
- Rendering of a node's own output (for a node editor thumbnail): prepare a copy of the grade
  with `output` set to that node and render the clip (or use `render::applyGrade()` on a still).
- Page widgets (colorui): `ColorNodeEditor`, `ColorWheel` / `PrimaryBars` / `PrimariesPanel`,
  `GradeCurveEditor` / `ColorCurvesPanel`, `QualifierPanel`, `WindowsPanel`, `LutBrowser`,
  `KeyPanel`, `ColorGallery`, `ColorClipStrip`, `ColorMiniTimeline`, `ColorViewerOverlay`,
  `ColorFrameSource`; `ScopesPanel` from src/ui/scopes.

## Left for later
Tracking of windows, polygon shape keyframes and Bezier windows, Midtone Detail, soft clip,
HDR / log wheels, key mixer / splitter / outside nodes, node presets and versions, ColorTrace
and grade groups, keyframe strip on the Color page, Sizing palette, node labels typed in the
editor, the qualifier's 3D / RGB / Luma modes, the lightbox, split-screen modes other than the
split wipe, relative LUT paths and packing LUTs into single-file projects, thumbnails of the
clip itself when a higher track covers it.
