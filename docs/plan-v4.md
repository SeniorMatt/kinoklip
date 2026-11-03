# Plan v4: projects and effects

Owner decision 2026-11-03: the Color and Compositing pages move to v5; v4 is about managing
projects and about effects on the Edit page. Each part below is released on its own
(0.4.0, 0.4.1, ...), with spec rows in `docs/behavior-spec.md` and tests, like every earlier
milestone. After v4 Kinoklip goes public and releases become betas (roadmap.md). Everything follows [clean-room.md](clean-room.md): the Project Manager and the effects
behave the way a Resolve user expects, written from general knowledge, never from Resolve itself.

## Order
1. **Project Manager** first (owner: "first of all").
2. **Single-file projects** next. The low-level container (reading and writing the file, playing
   media from inside it) can be built at the same time as the Project Manager, because it lives in
   its own module; wiring it into opening and saving waits until the Project Manager is merged.
3. **Effects** in parallel with both, in batches; keying (green screen) is the first batch.
4. v3 leftovers fit in between when there is a free slot.

## 1. Project Manager (0.4.0)
What a Resolve user expects when the app starts and when they open "Project Manager":
- **A project library**: one place where Kinoklip keeps its projects (default
  `~/Videos/Kinoklip Projects`, changeable in Preferences; more than one library allowed later).
  Projects saved elsewhere still open and show up under "Recent".
- **Window**: shown at start-up instead of an empty Edit page (option: skip it and reopen the last
  project), and from File > Project Manager (Shift+1 in the Resolve keymap). Thumbnail cards
  (poster frame of the timeline, name, last modified, duration, resolution and frame rate) or a
  list view with sortable columns; search; sort by name / date; zoom of the cards.
- **Folders** inside the library to organise projects; drag projects between folders.
- **Actions**: New Project (asks for a name), Open, Open in a new window later, Rename, Duplicate,
  Delete (to the system trash, with confirmation), Show in File Manager, Import (copy a project
  file into the library), Export (save a copy elsewhere, optionally as a single file with media,
  see part 2), Project Settings.
- **Safety**: an open project cannot be deleted; unsaved-changes prompt when switching; missing
  media marked on the card; a broken project file shows an error card instead of breaking the list.
- **Speed**: cards load lazily, thumbnails are cached; a library with hundreds of projects opens
  instantly.

## 2. Single-file projects (0.4.x)
The owner wants a project that is **one file** containing everything (timelines and every video,
image and sound), to move around like any other file.
- **Format**: one container file (extension decided in the design step, e.g. `.kinoklip`) that holds
  the project data and the media files unchanged. Requirements:
  - Media plays **straight from inside the file**, without unpacking (read by offset through
    FFmpeg's custom I/O), so a 50 GB project opens instantly.
  - Saving does not rewrite the whole file: the project data is small and appended; media is only
    added when imported.
  - A crash or power loss during a save never loses the previous saved state.
  - Files over 4 GB, media files of any size.
  - Prefer an open, documented layout (for example ZIP without compression, ZIP64) so the media can
    be recovered with standard tools even without Kinoklip.
  - Caches (thumbnails, waveforms, proxies, render cache) are not stored inside; they are rebuilt.
- **Owner decision 2026-11-03:** New Project asks which kind to create, with **single file as
  the default** (the choice is remembered); either kind converts to the other later. In a
  single-file project the Media Pool bins are the only organisation needed: clips live inside the
  file, so there are no folders on disk to keep in order.
- **Two kinds of project**, chosen per project: **linked** (today's `.kkproj`: media stays where it
  is) and **self-contained** (the single file). Conversions both ways:
  - **Pack**: copy every used media file into the project file (Project Manager > Export as single
    file, and File > Pack Project). Optionally only the used parts of long clips, with handles.
  - **Unpack**: write the media out to a folder and turn the project back into a linked one.
- **Compact**: remove media no longer used by any timeline and old project data left behind by
  earlier saves, with a size preview before and after.
- **Import into a self-contained project** copies the media into the file (with progress, and
  cancellable); the Media Pool shows where each clip lives.
- Status (kkpack branch): the container library, in-place reading and the Pack / Unpack / Add
  media / Compact / Verify functions are done (docs/single-file-project.md, spec section 39); the
  UI integration (Project Manager, File menu, import, Media Pool indicator) is the next step.
  TODO: Pack with trimming (used parts of long clips plus handles: needs re-muxing per container
  and keyframe-aware cut points); image sequences inside the file.

## 3. Effects on the Edit page (0.4.x, in batches)
Today the Edit page has 8 video effects (Brightness / Contrast, Saturation, Gaussian Blur, Sharpen,
Vignette, Mosaic, Mirror / Flip, Drop Shadow). Goal: a broad effects library like the one a
Resolve user knows from the Edit page, each effect
- applied from the Effects panel by drag and drop onto a clip, edited in the Inspector, with every
  parameter keyframable;
- rendered on the GPU for live playback and with an identical result in the render (the same
  shader path, or a CPU path tested against it);
- covered by tests (parameter defaults, a reference image per effect, project save / load).

Batches, in order:
1. **Keying** (first, owner request): Chroma Key / green and blue screen (key colour picker on the
   viewer, tolerance, softness, edge shrink / grow, spill removal, matte view), Luma Key, HSL Key,
   Difference Key; combined with the existing masks as garbage mattes.
2. **Colour**: Exposure, Temperature / Tint, Levels, Curves, HSL adjust, Color Balance (lift /
   gamma / gain), Invert, Black and White / Tint, Posterize, apply a LUT file (.cube).
3. **Blur and sharpen**: Box, Directional, Radial, Zoom, Lens blur, Tilt-Shift, Unsharp Mask,
   Noise Reduction (simple).
4. **Light and look**: Glow, Light Rays, Lens Flare, Film Grain, Halation, Bloom.
5. **Stylize**: Edge Detect, Emboss, Pixelate variants, Halftone, Scanlines, Pencil Sketch,
   Abstraction, Prism, Chromatic Aberration.
6. **Distort and transform**: Lens Distortion, Ripple, Wave, Twirl / Vortex, Warp, Camera Shake,
   Crop with feather, Stroke / Border, Corner Pin.
7. **Time**: Echo / Ghosting, Strobe, Freeze Frame, Flicker.

## 4. Viewer guides (owner request 2026-11-03)
Guides on the viewer that help placing things but never appear in the render, like the ones a
Premiere Pro user knows: horizontal and vertical guide lines added from the View menu or dragged
out of rulers along the viewer edges, moved and removed by dragging, snapping for on-screen
transforms, lock, clear, colour; plus safe-area overlays (action / title safe) and a grid. Saved
with the project per timeline.

## v3 leftovers (when a slot is free)
Duration typing in the Edit Index (A15), AAF / FCPXML interchange (A16), cached clip names (A17),
the program-look two-up, the 4K HEVC first-play drops.
