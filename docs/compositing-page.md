# Compositing page (design)

Owner decision 2026-11-03: work on the Compositing page now (it was planned for v5). It is
Kinoklip's node-based compositing page, our own name for what a Resolve user knows as the page
between Edit and Color. Clean room ([clean-room.md](clean-room.md)): behaviour and layout from
general knowledge of node compositors, our own drawing, names and icons; never Resolve itself.

## What it is for the user
- Every video clip on the timeline can get a **composition**: a node graph whose result replaces
  the clip's picture. A new composition is `MediaIn1 -> MediaOut1` (the clip's own picture
  straight through), so opening a clip on the Compositing page changes nothing until you add
  nodes.
- Open: select a clip (or put the playhead over it) and switch to the Compositing page
  (page bar, Shift+5 in the Resolve keymap), or the timeline clip menu > Open in Compositing Page.
  Clips with a composition show a small badge on the Edit page timeline; Clip > Reset
  Composition removes it.
- The result plays in the Edit page and renders on the Deliver page exactly as on the
  Compositing page (one evaluation path).

## Page layout
- Top: **two viewers** side by side (left, right). Each shows the output of one node: select a
  node and press **1** (left) or **2** (right), or drag the node onto a viewer. Viewer header:
  node name, zoom, and the overlays of the selected node (transform handles for Transform and
  Merge, shape points for masks), reusing the Edit page viewer overlays and guides where they fit.
- Under the viewers: a **time ruler and transport** over the composition's frames (the clip's
  duration), with keyframe ticks of the selected node, and the comp frame / timeline timecode.
- Bottom: the **node editor**: pannable, zoomable canvas (middle-drag or Space-drag to pan,
  wheel to zoom, Fit with Shift+F), nodes as tiles with input ports on the left / top and the output
  on the right, coloured links by port kind (background, foreground, mask), selection and box
  selection, drag to move, drop a node onto a link to insert it, drag from a port to connect, drop an output on another
  node's output to put a Merge there (the dragged node is the foreground),
  drag a link end away to disconnect, Delete, Ctrl+C / Ctrl+V, Ctrl+Z, notes.
- Above the node editor: a **toolbar** with the common nodes (Background, Text, Merge,
  Transform, Rectangle, Ellipse, Polygon, Blur, Color Corrector, Chroma Key); clicking adds the node
  after the selected one. **Shift+Space** opens a searchable "Select Node" list of every node.
- Right: the **Inspector** for the selected node (same parameter rows and keyframe buttons as the
  Edit page Inspector).
- Left (toggle): the **Nodes library** (every node by category; drag onto the node editor).
- Interface bar toggles like the other pages (Nodes, Spline, Keyframes, Inspector). **Spline**
  and **Keyframes** open `KeyframesPanel` in composition mode beside the node editor (the Curves
  view and the dope sheet of the selected nodes' animated parameters); see
  src/ui/keyframes/readme.md.
- Keys: the node editor claims its own keys while it has focus (Delete, Ctrl+C / V / D, F2,
  Ctrl+P, Ctrl+T, 1 / 2, Shift+Space); Keyboard Customization treats them as a separate scope,
  so they never conflict with the Edit page's. The Edit page timeline is hidden here, so its
  editing commands are disabled while the page is shown; transport, undo and the page keys stay
  (TimelineCommandSuspension, shared with the Media and Deliver pages). The Select Node list
  opens at the mouse but stays inside the window like a menu.

## Nodes (first release)
- **I/O**: MediaIn (the clip, or any media pool item), MediaOut.
- **Composite**: Merge (background, foreground, mask; blend mode (Normal, Screen, Multiply,
  Add, Overlay, Darken, Lighten, Difference, Subtract), opacity, foreground centre / pivot / size / angle, flip horizontal / vertical, edges, filter method, invert transform, alpha gain, burn in, subtractive / additive, reference size; 33 apply modes and the Normal operators Over / In / Held Out / Atop / XOr: spec section 88),
  Dissolve.
- **Transform**: Transform (centre, size, angle, pivot, flip), Crop, Resize, Letterbox.
- **Generate**: Background (solid colour or 2/4-colour gradient), Text (font, size, colour,
  outline, shadow, position), Noise / Fast Noise.
- **Masks**: Rectangle (with Corner Radius), Ellipse, Polygon, B-Spline (points with weights, animated as one
  shape); each with soft edge, invert, level, paint mode, centre / size / angle; masks plug into any node's mask
  input (the Paint Mode says how a mask joins the one plugged into it). Spec section 92.
- **Matte**: Luma Keyer (channel, low / high, soft range, invert), Delta Keyer (screen colour key with gain,
  balance, pre-matte soft range, clean background / foreground, matte thresholds and despill).
- **Corner Positioner** (Transform): four corner points and a perspective warp, with draggable corner handles in
  the viewer.
- **Effects**: every Edit page video effect (62 today: keys, colour, blur, light, stylize,
  distort, time) as a node with one image input and a mask input, generated from the
  EffectRegistry (type `fx:<id>`), so new effects appear on both pages.
- **Color**: Color Corrector (the Color Balance / Levels / Saturation set in one node),
  Channel Booleans (copy / swap channels, alpha from luma).
- **Tracking**: Tracker (point tracker, see "Tracker" below), Planar Tracker.
- Later: B-spline / paint, particles, 3D, expressions, modifiers.

## Data model and evaluation
- `src/core/Composition.h`: value types (Composition, CompNode, NodeLink, CompNote). A clip
  holds `std::optional<Composition> composition` in ClipData. Project format bump (15) with
  migration (older files: no compositions).
- `src/core/NodeRegistry.*`: node descriptors (id, name, category, input ports with kind and
  required flag, outputs, parameters reusing `ParamDescriptor`); effects as nodes come from the
  EffectRegistry.
- `src/core/CompOps.*`: pure operations on a Composition (default composition, add / remove /
  connect / disconnect / move / rename / insert-into-link / paste, unique names, cycle check)
  and one undoable Project call that replaces a clip's composition ("Add Node", "Connect", ...).
- `src/render`: GPU evaluation of a composition for a comp frame: pull from MediaOut (or a
  viewer's node), each node one pass over textures in the timeline resolution with a domain of
  definition where cheap; per-frame texture cache; MediaIn decodes through the existing decoder
  pool; effect nodes reuse the GlEffects passes. The clip's composition output feeds the
  existing clip pipeline (effects, masks, transform, compositing), so the Edit page preview,
  playback and the render all use it. Viewer API: render the image of any node at a comp frame.
- Undo: every graph or parameter change is one undo step in the project's history.

## Order of work
1. Core (model in ClipData, registry, ops, serialization, evaluation, render integration) and
   the page UI (layout, node editor, viewers, inspector hookup) in parallel against
   `Composition.h` and this document.
2. Integration, end-to-end tests (comp in the render), release.
3. More nodes and tools (planar tracker, paint, particles, 3D).

## Tracker (point tracker, issue #3)

**What the user does.** Add a Tracker node (Transform folder) after the picture that should
move. Select it: the viewer shows the tracked point with its **pattern box** (the detail to
follow, solid) and **search box** (how far it may move between two frames, dashed); drag the
cross to place the point, the corners to resize the boxes. Put the playhead on the first frame
and press **Track Forward** (or **Track Backward** to go the other way) in the Inspector; the
playhead follows, a progress bar shows how far it is and **Stop** ends it early (what is
tracked so far is kept). The result is stored as keyframes on the node's Center X / Center Y,
in one undo step; **Clear** removes them. A track stops where the pattern is lost, with a
message.

**What the node outputs.** The Tracker passes its *Input* picture through, moved by the track
relative to the **Reference Frame** (the frame the first track started on): *Match Move* moves
the input with the tracked point (an overlay stuck to a moving object); *Stabilize* moves it
the opposite way (the shaky shot held steady). The picture that is analysed is the optional
**Tracking Source** input if it is connected (typically the plate), otherwise the Input. The
Tracking Source port is only read by the tracking, never drawn, so it costs nothing at render
time. Because the track is plain keyframed X / Y, it can be edited in the Keyframes panel or
by dragging the point on a frame, and (later) other nodes can link to it.

**How it works** (`src/compositing/tracking`, clean-room maths, no OpenCV).
`PointTracker` takes a grey picture, builds a half-size pyramid (the coarsest level keeps the
pattern at least about 9 px wide) and matches the pattern with normalised cross-correlation:
exhaustively over the search box on the coarsest level, then a few pixels around the
scaled-up result on each finer level. A Gauss-Newton step on bicubic samples then refines the
position to a fraction of a pixel. Normalised correlation ignores brightness and contrast
changes. The pattern is taken from the first frame and re-taken from the current frame when
the correlation falls below 0.85, so a slowly changing target stays followed; below 0.6 the
point is lost. `TrackerRunner` feeds it one frame at a time, off the UI thread: the pictures
are rendered by `PlaybackEngine::renderCompositionNode()` of the tracking source (the same
path as the viewers, so decoding, proxies and quality are not duplicated), the next picture
already rendering while the current one is matched. `storeTrack()` writes the result to the node.

## Planar Tracker and track links (1.2, issue #3)

**What the user does.** Add a Planar Tracker node (Transform folder) after the picture to pin or
to move, and connect the plate to its Tracking Source (or leave the Input as the plate). The viewer
shows a quad with four corner handles: drag the corners onto a flat, textured, fully visible
surface (a screen, a wall, a poster) on the first frame, or drag inside to move the whole quad.
**Track Forward / Backward / Stop / Clear** and the progress bar work as on the Tracker; the
corners are stored as keyframes of the eight corner parameters, in one undo step. **Pattern Size**
is the size of the patches that are followed, **Search Size** how far a patch may move between
two frames. A track stops where the surface is lost (hidden, blurred, out of the picture).

**What the node outputs** (Output): *Corner Pin* draws the Input with its four corners on the
tracked corners (perspective), over transparency, so a Merge puts it onto the plate; *Unsteady*
moves, turns and scales the Input the way the plane did since the **Reference Frame**; *Steady*
does the opposite (the plane held still). Unsteady / Steady use the best-fit similarity of the
four corners, so they are the position, rotation and scale of the plane without the perspective.

**How it works** (`src/compositing/tracking`). `PlanarTracker` lays a grid of 7 x 7 patches on the
region (spread over the surface through the homography of the unit square, so a region in
perspective gets an even spread) and keeps those with enough contrast. For every new frame the
patches are cut from the start frame warped by the previous frame's homography (so rotation, scale
and perspective do not spoil the match), searched with the point tracker's matcher (normalised
cross-correlation, a half-size search first, then full size, then the sub-pixel Gauss-Newton step;
`PatchMatch.h`), and `estimateHomography()` (`Homography.h`: RANSAC over 4-point samples, normalised
DLT, refit on the inliers) turns the matches into the plane's homography. A second pass with the
patches re-warped by that result, a small radius and a stricter correlation limit refines it and
drops patches that are partly covered by something in front of the plane. The patches come from the
start frame (no drift) and are re-cut from the current frame when the mean correlation falls
below 0.85. The corners are the homography applied to the start corners; a result that is not a
convex quad, or whose area changed more than 40 times, or with too few agreeing patches, is a loss.
`TrackerRunner` runs it exactly as it runs the point tracker (same thread, same pictures, same
progress and stop); a job with `planar` set carries the four corners.

**Linking other nodes to a track.** Kinoklip has no parameter links: every parameter is a constant
or keyframes. The smallest mechanism that fits is *Connect to Tracker n*: nodes with a Center
(Transform, Merge, Text, the shape masks, and so on) have a "Follow Track" row in the Inspector;
Connect copies the chosen track (a Tracker's point, or the centre of a Planar Tracker's region)
into the node's Center X / Y keyframes at the frames of the track, with a fixed offset that keeps
the node where it is at the playhead, in one undo step (`tracking::linkToTrack()`). It is a copy,
not a live link: after tracking again, press Connect again. Because the result is plain keyframes
it can be edited in the Keyframes panel and needs no change to the data model, the project file or
the renderers.

**Not done yet.** Polygon regions (a bounding plane from more than four points), motion
prediction for very fast movement, per-frame manual correction with re-tracking from the
corrected frame, a live (re-resolving) link, linking sizes and angles, and tracking of masks.

## Node editor extras (1.7, spec 93)

Insert on links (Shift-drag of a connected tile; library drops already did), tile pictures
(`CompNode::thumbnail`, rendered by a page-supplied `NodeEditor::ThumbnailSource` through
`PlaybackEngine::renderCompositionNode()`, throttled), hover tooltip, Replace With
(`compops::replaceType()`), Find (Ctrl+F, panel-local), Set Color (`CompNode::color`), Paste
Settings (Ctrl+Shift+V, panel-local) and Shift-drag on a link to disconnect it. Colour and
picture flag are written only when set; the project format stays 19. Fit Nodes to View is
Shift+F now (Ctrl+F is Find).
