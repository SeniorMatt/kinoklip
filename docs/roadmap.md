# Kinoklip roadmap

What is planned, in order. Nothing here is a promise: the owner decides what starts and when a
version is released. Finished milestones (v1 to v4) and the old plans are in [archive/](archive/).
Bugs and ideas from users: [GitHub issues](https://github.com/SeniorMatt/kinoklip/issues).

Rules for all work: pages and features get their own names (no Blackmagic product names), and
everything follows the [clean-room policy](clean-room.md).

Every feature is one checklist line, `- [x]` when made and `- [ ]` when not, with its area before the first colon. `python3 scripts/status.py` counts the lines (and the table in [effects-library-status.md](effects-library-status.md)) and draws the picture in the readme.

## Version 1.2 (released 2026-11-11 as 1.2 Beta)

Everything below in "Now" is planned for 1.2, in this order: OpenTimelineIO, planar tracker, audio effects, the command-line interface and the MCP server on top of it, then the missing transitions, effects and generators (Smooth Cut first), stabilization, fade curves, hover preview, the text title layers, emoji, Inspector audio options, settings import/export and vim keys. 1.1.8 has the small fixes (search box, Keyframes / Curves switch, licence texts, FUSE note).

## Now (next version)

User requests come first: they are the most valuable tasks.

Shipped in 1.1.7:
- [x] Playback: thumbnails give way to playback ([#2](https://github.com/SeniorMatt/kinoklip/issues/2))
- [x] Tracking: the point tracker ([#3](https://github.com/SeniorMatt/kinoklip/issues/3))
- [x] Edit: Close Gap and Ripple Delete on gaps

Shipped in 1.1.8:
- [x] Media: the thumbnail slider no longer squeezes the search box at the default width
- [x] Interface: the Keyframes / Curves switch looks like native Breeze / Qt
- [x] Platform: complete licence texts for every bundled library

Shipped in 1.2 ([release notes](releases/v1.2.0-beta.1.md)):
- [x] Interchange: OpenTimelineIO import and export ([#5](https://github.com/SeniorMatt/kinoklip/issues/5))
- [x] Tracking: the planar tracker and Follow Track
- [x] Audio: 8 audio effects on audio clips (Gain, Equalizer, Compressor, Limiter, High-pass, Low-pass, Delay, Reverb)
- [x] Automation: the command-line interface
- [x] Automation: the MCP server, with editing commands for effects, titles, keyframes and tracks, and optional live control of the open window
- [x] Effects: Smooth Cut and 56 more video transitions (61 in all)
- [x] Effects: 9 more generators and test patterns (18 in all)
- [x] Effects: 28 more video effects (90 in all) and the Colored Border, Binoculars and Night Vision looks
- [x] Timeline: fade curves that are shaped with the mouse on the clip
- [x] Tracking: stabilization in the Inspector (Perspective, Similarity and Translation modes, Camera Lock, Zoom) and from the command line

Still to do for 1.2:
- [ ] Platform: AppImage shows an error when FUSE is missing (today it fails silently)
- [x] Media: hovering a clip thumbnail in the Media Pool previews it in the viewer, as in Resolve (spec 66)
- [x] Effects: Motion Trails
- [x] Effects: Smear
- [x] Effects: Frame Replacer
- [x] Effects: Motion Blur
- [x] Effects: Automatic Dirt Removal
- [x] Effects: Deflicker
- [x] Effects: Color Stabilizer
- [x] Effects: Dust Buster
- [x] Effects: Patch Replacer
- [x] Tracking: Match Move
- [x] Tracking: Surface Tracker
- [x] Effects: Warper
- [x] Effects: CCTV look
- [x] Effects: Digital Glitch look
- [x] Effects: Drone Overlay look
- [x] Effects: DSLR look
- [x] Pages: holding a drag over a page button for a moment switches to that page (spec 71)
- [x] Effects: DVE look
- [x] Effects: Video Call look
- [x] Effects: Video Camera look
- [x] Titles: more options on the existing text title, mainly layers (several text and shape layers in one title); no new title type
- [x] Titles: emoji in subtitles and titles, in colour, in the viewer and in exports
- [x] Audio: Inspector Audio tab compared with every audio option Resolve has (public documentation only), adding the missing ones, Pitch first (spec 67: Pitch and a four-band clip Equalizer added; Voice Isolation and Dialogue Leveler skipped, they need a model)
- [x] Interface: import and export all settings with one click
- [x] Interface: vim keybindings, so nvim users can edit easily
- [x] Interface: Help > Export Log File: saves what the app did in this session (messages, warnings, the decoder chosen for each file, the graphics card, the playback statistics) as one text file to attach to a bug report. The log is kept in memory while you work and written only when you ask; the dialog says what the file contains (owner pick, idea from Picasso Suite)

## Version 1.3.1 (released 2026-11-11)

Shipped in 1.3.1 ([release notes](releases/v1.3.1.md)): Remove from Recent Projects, cut lag after pressing Play, decoder line in the exported log.

## Version 1.3 (released 2026-11-11 as 1.3 Beta)

Shipped in 1.3 ([release notes](releases/v1.3.0-beta.1.md)):

- [x] Interface: every window and dialog opens at least big enough for its content, so no scrollbars appear when they are not needed (Project Settings first)
- [x] Audio: voice-over: record from the microphone straight onto an audio track while the timeline plays. An arm button and an input menu on the track header (Edit and Audio pages), an input level meter, a count-in, and each take saved as a WAV file beside the project and added to the Media Pool (owner pick) (spec 75; the capture itself could not be tested without a microphone)
- [x] Interface: dragging files over a closed panel's toolbar button (Media Pool, Effects and every other panel that takes files) opens that panel after the same hover delay as the page buttons, so the drop can land in it

## Version 1.4 (released 2026-11-11)

Shipped in 1.4 ([release notes](releases/v1.4.0.md)):

- [x] Effects: Background Removal: an optional, clearly labelled AI tool in the Effects library (Key) that cuts the person out of a clip with a model that runs on your computer (Robust Video Matting, GPL-3.0, through ONNX Runtime, MIT). The model is downloaded only when you ask (name, size, licence and source shown first, checked against a checksum, removable in Preferences > AI Models); mattes are made in the background and cached, playback never waits, the render does (spec 78, [guide](guide/remove-a-background.md))
- [x] Media Pool: Ctrl+V imports files or a picture from the clipboard into the current bin (spec 79)
- [x] Playback: a prepared cut also holds its next 3 frames, so the first frames after a cut are not decoded late (spec 72.7)

## Version 1.5 (released 2026-11-12)

- [x] Interface: Vim mode is an option (Preferences > Keyboard, Kinoklip > Vim Mode), no longer a keyboard preset: a permanent mode line in the page bar row, INSERT mode, the Esc rule, the focused-panel frame, and the old Vim profile migrated (spec 83, step 1)
- [x] Interface: Vim keys as a table: per-context key sequences with timeout, your own keys in Keyboard Customization > Vim keys, a vimrc file, :nmap, which-key list, keys by physical position (a Russian layout works) (spec 83, step 2)
- [x] Interface: Vim grammar on the timeline (cursor track, operators with motions and text objects, registers, dot repeat, marks, Visual modes, search) (spec 83, step 3)
- [x] Interface: Vim command line with completion and history, Telescope-style pickers on the leader (spec 83, steps 4)
- [x] Interface: Vim keys in every panel: windows and pages, lists and trees, the Inspector and other forms, the keyframe editor, node graphs, the mixer and transitions (spec 83, steps 5 to 7)
- [x] Interface: Vim macros (q, @, @@, @:), append and numbered registers, ? backward search, f F t T ; , to find clips by name, * and #, a check of the whole key table, the guide "Vim for Kinoklip" and a one-page cheat sheet (spec 83, step 8).

## Version 1.6 (released 2026-11-12)

- [x] Compositing: a Spline panel (Fusion-style curve editor for every animated node value) and a Keyframes panel on the page, with keyframe loops: Loop, Ping-Pong and Relative (repeat with offset) before and after the keys, Reverse and Time Stretch; the loop commands also in the Edit page's Curves view
- [x] Compositing: dropping an output onto a connected node adds a Merge; Merge has Flip Horizontal and Flip Vertical
- [x] Edit: change Transform and Crop of several selected clips at once (Inspector and viewer handles); copy transitions with all their settings (Alt-drag, Ctrl+C / Ctrl+V) (spec 179.1, 24.1, 24.11, 13.17, 13.20)
- [ ] Edit: audio plays after a big paste without a restart (guarded by tests, spec 16.8; the owner's failure was never reproduced: needs the owner's project)
- [x] Background Removal: a general model for pictures and any subject (BiRefNet)
- [x] Media Pool: Save a Copy to Computer for media stored in a single-file project
- [x] Timeline: thumbnails come back quickly after zooming
- [x] Fonts: newly installed fonts show up without restarting (automatic)
- [x] Audio: a visual equalizer editor (curve with draggable bands)
- [x] Timeline Settings: change a timeline's or compound clip's frame rate after clips are in it (clips keep their time)
- [x] Titles: Write On (typewriter) for titles and the Text node, and a Typewriter title
- [x] Project Settings: the standard transition and its length are project settings (Set as Standard Transition in the Effects library); Preferences: default type for new keyframes
- [x] Render: memory stays flat with keyframed generators; a render pauses instead of being killed when memory runs out
- [x] Keyframes panel fits small screens; Vim Mode in Preferences only; title typing no longer flickers; transitions from the library land on the cut

## Version 1.7: the Compositing page

- [x] Compositing: Merge and Transform controls in full (Edges, Pivot, Invert Transform, Alpha Gain, Burn In, all apply modes)
- [x] Compositing: the rest of Fusion's node editor behaviour (insert on a link, add after the selected node, swap inputs), checked against Resolve's documentation
- [x] Compositing: viewer features (A/B split, channel keys, checker, show/hide controls, lock)
- [x] Compositing: Common Controls tab on every tool (Blend, RGBA, mask options, motion blur)
- [x] Compositing: more tools (Delta Keyer, Luma Keyer, Corner Positioner, B-Spline mask, rounded Rectangle) and node editor extras (thumbnails, Replace, Find, tile colours, Paste Settings)
- [ ] Background Removal: a newer video model if one fits our licence
- [ ] Bug hunt for 1.7: look for bugs like the ones the owner found on 2026-11-12 and fix them. Look for: the viewer not following what was clicked; mouse buttons and keys that do nothing where Resolve acts; panels and tool rows overlapping or cutting off names on small screens; keys that take over other keys; missing search or defaults that differ from Resolve; effects and niche controls Resolve has that are missing or behave differently

## 1.8.2 (released 2026-11-16)

- [x] Projects with duplicate effect ids open again; cut, pasted and multicam clips get their own effect ids
- [x] A damaged timeline always opens: it is repaired on load and a message lists what changed
- [x] Ripple Delete, Ripple Cut and Ripple Overwrite keep every sync-locked track in sync
- [x] Ripple trims on a clip's start keep the other tracks over the same picture

## 1.8.1 (released 2026-11-14)

- [x] Playback: memory stays bounded in slow-motion playback ([#8](https://github.com/SeniorMatt/kinoklip/issues/8))
- [x] Compositing: page sound: Audio button, MediaIn Audio (Timeline / Solo this clip) and Sound Offset ([#7](https://github.com/SeniorMatt/kinoklip/issues/7))
- [x] Edit: effects and transitions go to every selected clip
- [x] Edit: Shift held during a timeline drag flips snapping
- [x] Edit: Preferences > Editing "Sync Lock on new tracks"
- [x] Edit: Ctrl+Tab and Ctrl+Shift+Tab switch timeline tabs
- [x] Edit: ripple trims keep sync-locked tracks in sync on every path
- [x] Effects: Add Effect search lists name matches first
- [x] Media: one Media Pool row per mouse wheel notch
- [x] Preferences: explanations on hover, one restart message after Save

## Next version (after 1.8.x)

- [x] Edit: ripple audit: every ripple delete, ripple cut and ripple trim entry point (Edit and Cut page, mouse, keys, menus) keeps all sync-locked tracks in sync (spec 145.1; test_rippleaudit drives each entry point on one timeline: keys and menu actions, Trim tool and Cut page mouse drags, several edit points, duration, speed, paste insert, range delete, Cut page smart edits)
- [x] Edit: Alt-drag copy of a clip to another track shows up there (spec 145.2; test_parity spec7_3_altDragCopyToAnotherTrackShowsThere)
- [x] Platform: AppImage for ARM64 on every release (`Kinoklip-aarch64.AppImage`, built by the `arm64-release.yml` workflow) (user request)
- [ ] Playback: 10-bit HEVC (4K drone footage) decodes on Intel graphics without copying each frame to system memory (code and unit tests done: P010 import, compression metadata planes for Xe2, layout log; open until a Lunar Lake owner confirms, the build container has no readable render node)
- [x] Edit: several edit points selected and trimmed together (Ctrl+click edit points, trim with , . or the mouse, spec 104; test_multiedit)
- [x] Performance: Kinoklip uses less memory (measure what holds RAM and VRAM while editing and playing: frame caches, decoders, thumbnails, waveforms, undo history), so it runs well on low-end computers (memory profile test; caches scale below 8 GB, Preferences > System > Memory and GPU > Limit Kinoklip memory usage to; same session on llvmpipe: 1601 MiB resident after 16 s idle before, 574 MiB after; peak 1597 before, 1476 after)
- [x] Tests: quick, full and release test groups, so day-to-day builds run only the quick tests
- [x] Platform: dev builds: a test AppImage built on our own computer (not on GitHub servers) with the quick and full tests, for trying new work before a release

## Version 1.10.0 (released 2026-11-18)

- [x] Color: Tracker palette: a window follows a tracked subject (Pan, Tilt, Zoom, Rotate, 3D) so a blurred or graded face stays covered, and a Stabilizer mode of the palette (spec 107; Interactive mode and the Cloud tracker are not done)
- [x] Color: Curve (Bezier) power window in the Windows palette, with polygon and curve shape keyframes
- [x] Color: Qualifier Matte Finesse (Clean Black / White, Blur Radius, Shrink / Grow, In / Out Ratio, Denoise) and the 3D, RGB and Luma qualifier modes (spec 108; same maths on GPU and CPU, saved in the project, one undo step each; the qualifier stays static, so no keyframes)
- [x] App: welcome window on first start (and once after an update) that asks the choices that differ from DaVinci Resolve: language, colour theme, scale, menu layout, keyboard profile, Vim mode, timeline habits, hardware decoding; Help > Welcome opens it again (spec 110)
- [x] Color: RGB Mixer palette (Red / Green / Blue output sliders, Monochrome, Preserve Luminance; spec 109; keyframes, undo, same maths on GPU and CPU)
- [x] Color: Blur / Sharpen / Mist palette on the Color page, so a window can blur part of the picture (spec 106; Radius R G B with link, H/V Ratio, Scaling, Coring Softness, Level, Mist Mix; keyframes, undo, same maths on GPU and CPU; tracking a window is the Tracker palette, spec 107)

## Version 1.11.0 (released 2026-11-19)

New default look that follows DaVinci Resolve 21 (Kinoklip widget style and colour scheme, Open Sans; Breeze stays selectable), the Audio page work, the Compositing nodes and the live Color page. [Release notes](releases/v1.11.0.md).

- [x] Compositing: Text+ controls: Layout (Point, Frame, Circle, Path), Tracking, Line Spacing, Kerning, Shading elements, a Write On range on the Text node, and a Follower for per-character animation
- [x] Compositing: Bezier mask with tangent handles and Roto assist, Point Follower, and mask tracking (a Mask Tracker moves a mask, polygon points, size and angle; also on the Edit page)
- [x] Compositing: Planar Tracker Track Mask input, several trackers in one Tracker node (rotation and scale from two patterns), and Follow Track for size and angle
- [x] Compositing: Ultra Keyer, Delta Keyer advanced tabs (Pre-Matte Blur, Erode / Dilate, Matte Blur, Fringe, Replace Mode), Matte Control (Garbage and Solid masks, Restore Fringe) and a Clean Plate input
- [x] Compositing: Light Wrap and Soft Glow nodes
- [x] Compositing: particles: pEmitter, pRender, pMerge, pGravity, pTurbulence (2D output; more forces, mask regions and 3D output are open, spec 129.10)
- [x] Compositing: 3D: Image Plane 3D, Text3D (flat or extruded with bevelled edges, Fusion-style Extrusion Depth / Bevel controls, separate front / bevel / side / back colours), Shape3D, Merge3D, Camera3D, lights, Renderer3D and a 3D viewer (software rasterizer on both renderers; shadows, 3D particles, importing models are open, spec 130.11)
- [x] Compositing: Paint node (Stroke, Clone, Eraser, Smear tools, brush size / softness / opacity / colour, stroke duration, strokes saved as vector points with pressure, drawn in the viewer; Fill, Stroke mode and Emboss are open, spec 131.10)
- [x] Compositing: modifiers (Perturb, Wiggle, Offset, Expression, Cycle, XY Path), parameter links with Connect To and Publish, and expressions in value fields
- [x] Compositing: Group, Macro (Make Macro, published controls, save as a title in the Effects Library), Underlay and Instance nodes
- [x] Compositing: Grid Warp, Displace and Vector Distort nodes
- [x] Compositing: build a transition on the page (MediaIn1 / MediaIn2 composite), Loader and Saver nodes, Time Speed and Time Stretcher nodes
- [x] Compositing: Vector Motion Blur, Custom Tool (pixel expressions), Color Curves, Hue Curves, Brightness Contrast, Alpha Multiply / Divide and Channel Mapper nodes
- [x] Compositing: viewer Onion Skin, Region of Interest, Rulers, safe area overlay, and Proxy / Auto Proxy / High Quality / Motion Blur toggle buttons with a disk cache
- [x] Audio: files with several audio tracks (an OBS recording with game, mic and Discord): every stream becomes its own linked audio clip on consecutive tracks on every kind of edit, the Media Pool and the metadata show the track count, Clip Attributes chooses the tracks, the project stores the stream per clip, and the waveforms, meters, Audio page and render play the right one (spec 126)
- [x] Audio: noise reduction with a local, open model (RNNoise, BSD-3-Clause, built in; spectral gate with Manual Learn; DeepFilterNet left open; spec 132)
- [x] Audio: Effects Library and Inspector on the Audio page, effect slots on mixer tracks (tracks only; buses left open; spec 133)
- [x] Audio: Normalize a whole track or bus / the main mix to a loudness target in one step (right-click a mixer strip or a track header, Normalize Audio Levels...; spec 134)
- [x] Audio: Voice Isolation button on a track and in the Inspector (local open model, optional) (spec 141: RNNoise plus a spectral gate, Amount, Optional AI)
- [x] Audio: Gate and Expander (with hold, hysteresis and key filters) in the Dynamics window, a De-Esser effect, and a Dialogue Leveler (spec 136)
- [x] Audio: sidechain input on the compressor and gate (key from another track or bus, loops refused) for ducking music under a voice (spec 136)
- [x] Audio: Sends (level, Pre / Post fader, pan) to buses and aux buses, bus-to-bus feeds with loops refused, and a bus Pan (spec 137)
- [x] Audio: automation lane under a track with Volume, Pan, Mute, send and effect parameter curves, plus Latch and Trim modes (spec 139)
- [x] Audio: loudness meter standards and targets (-14 YouTube, -16, -23 EBU R128, ATSC), Loudness Range (LRA) and a history graph, and normalize to a loudness target when rendering (spec 134)
- [x] Audio: Chorus, Flanger, Distortion and Stereo Width effects, and Reverb with Pre-delay, Decay and room presets
- [x] Audio: EQ presets (voice, music, telephone, your own) and a Phase Invert button on the strip (spec 137)
- [x] Audio: Spectrum analyzer and Phase / correlation meters, and monitor controls (Dim, Mono, Mute) on the Audio page (spec 141)
- [x] Project Settings: a Presets page (Save As, Load, Update, Delete, Import, Export), Save As User Default Config and Reset to factory default, so new projects start at your own resolution and frame rate (user request); the first-clip question also matches the resolution

## Next version (after 1.11.0)

Beginner and intermediate tutorial workflows for Resolve's Color, Fairlight and Fusion pages that fail today are listed in [color-audio-gaps.md](color-audio-gaps.md); each gap below has its line. Order: Compositing, Audio, Color, then the rest.

Done on main since 1.11.0:
- [x] Platform: Flatpak repository on GitHub Pages with a stable and a nightly branch for x86_64 and aarch64, signed, updated by every release and every dev build (see [Installing Kinoklip](install.md))
- [x] Interface: the optional Libadwaita (GNOME look) widget style is listed in Preferences when the separate style plugin is installed

Still to do:
- [ ] Compositing: Magic Mask / Object Removal in the node graph (optional local open model); not started: Background Removal's matting code (`src/matting`) only makes a whole-picture foreground matte of a media file and has no point or stroke prompts and no inpainting, so it cannot be reused with modest effort
- [x] Compositing: save a transition built on the page as a reusable preset in the Effects Library (User Presets, rename, delete), Saver output as OpenEXR (half / float, none / RLE / ZIP; PIZ and DWAA need the OpenEXR library), and a Saver in a transition graph (spec 128.10 - 128.13)
- [x] Color: Primaries Shad, Hi, Color Boost, Midtone Detail (MD) and an Auto Color button, and a Log palette (Shadow, Midtone, Highlight, Offset wheels with Low / High Range) (spec 146; keyframes, undo, same maths on GPU and CPU; Auto Color is levels per channel plus a midtone balance, not Resolve's exact analysis)
- [x] Color: Curves: Sat vs Lum curve, Soft Clip (Low, Low Soft, High, High Soft) and a Pick eyedropper that adds a colour's or luma's points from the viewer (spec 146)
- [x] Color: Color Warper palette: Hue - Sat and Chroma - Luma pin grids (Low / Medium / High resolution; select, drag, nudge, reset pins; same maths on GPU and CPU, saved in the project, one undo step per drag) (spec 147)
- [x] Color: HDR palette: Global, Shadow, Light, Highlight and Specular zones (wheel, exposure, saturation, range and falloff) and a Black Offset (keyframes, undo, same maths on GPU and CPU) (spec 147)
- [ ] Color: Motion Effects palette: temporal and spatial Noise Reduction with Better / Faster modes
- [ ] Color: Magic Mask palette: draw a stroke on a person or object and it follows (local open model, optional)
- [ ] Color: Color Match palette: Shot Match to another clip, and a colour chart
- [x] Color: Generate 3D LUT (CUBE) from a grade, in the clip strip's context menu with 17, 33 and 65 point cubes (spec 146; windows, qualifiers and blur are not part of a LUT)
- [x] Color: node types Outside, Key Mixer, Splitter / Combiner and Compound Node, typed node labels, and an Alpha Output node with key input and output ports
- [x] Color: Gallery albums, Memories A to Z, Power Grade albums, Import Still and Export Still (image and our own grade file; Resolve DRX is not read; spec 151)
- [ ] Color: Versions of a grade (Local and Remote), Groups with Pre-Clip and Post-Clip grades
- [x] Color: CIE Chromaticity scope (spectral locus, Rec. 709 / DCI-P3 / Rec. 2020 triangles, D65) and scope settings (Scale 0 - 1023 / 0 - 255 / percent / 0.0 - 1.0, Gain), remembered per scope (spec 146)
- [x] Color: Color Space Transform, ACES Transform, Gamut Mapping, Chromatic Adaptation and Color Compressor effects, DCTL and DCTL Transition (a .dctl file translated to GLSL; GPU only; spec 205 to 207)
- [ ] Color: Color Management in Project Settings (DaVinci YRGB Color Managed) and Input Color Space in Clip Attributes
- [ ] Color: Face Refinement, Depth Map, Relight and Sky Replacement effects (optional local open models)
- [ ] Color: Resolve FX palette: put an effect (Glow, Lens Blur, Film Grain, Halation, Film Look) on a node so its window or qualifier limits it
- [x] Color: Keyframes palette and strip on the Color page (a row per node, Input and Output Sizing, Static and Dynamic keys, dissolve types, Auto Keyframe, move / add / delete) and a Sizing palette (Zoom, Width, Height, Pan, Tilt, Rotate, Flip; Input Sizing is the clip's Inspector transform, Output Sizing the timeline's; Pitch and Yaw not yet) (spec 148)
- [x] Color: Split Screen modes (Selected Clips, Timeline), Lightbox and Output Blanking
- [ ] Background Removal: Keep / Remove brush to correct the model by hand
- [x] Media: New Fusion Clip, and Fusion / compound clips as Media Pool entries you can drag to the timeline (one shared graph per Fusion clip, project format 24; test_fusionclip, test_fusionclipui)
- [x] Titles: underline and strikethrough (Title and Text node Inspector, drawn as bars under spaces too, Write On and Text+ aware, saved with the project; spec 145.3; test_titledecor)

## Version 1.13 (planned)

User requests first; the open lines of the section above carry over.

- [x] Deliver: HDR export (user request), first part: Rec.2020 colour space tag and PQ / HLG gamma tag in Render Settings, 10-bit H.265, AV1 (software encoders) and ProRes 422 / 4444 with the colour tags, HDR10 mastering display (P3-D65 or Rec.2020 primaries) and MaxCLL / MaxFALL, saved in settings and presets; the files are checked with ffprobe (spec 185; test_hdrexport, test_hdrfile)
- [ ] Deliver: HDR export, rest: the render path keeps HDR footage HDR (half-float or 10-bit pipeline; today an SDR picture is placed in the PQ / HLG container at 203 nits white), MaxCLL / MaxFALL measured from the render, hardware 10-bit encoders (P010), the timeline colour space in Project Settings, a viewer that shows the HDR picture on an HDR monitor (Wayland colour management) or tone-maps it for SDR, scopes in nits (PQ scale)
- [x] Media Pool: right-click > 3D LUT sets a LUT on a clip's media (None, the Color page's LUT library, Browse...), also on the Color page clip thumbnails; it is applied before the node graph in the Media Pool poster, every viewer, render and export, GPU and CPU alike, undoable, saved as an optional key (user request; spec 148.1 - 148.3)
- [x] Metadata panel: "Show All Metadata" in its options menu lists everything in the file (container and stream properties, all tags including non-standard ones such as `com.dji.camera.ColorGammaSxS : D-Log`, chapters, side data), searchable and copyable (user request; spec 148.4)
- [x] Media Pool: Relink with a preview table that matches files by other case, other extension, a find / replace pattern or the closest name, one undo step (user request; spec 148.5, 148.6)

## Plugins (branch `plugins`, after 1.14.1)

The plugin system: [design](plugins/design.md), [writing plugins](plugins/writing-plugins.md), [installing plugins](plugins/installing-plugins.md), spec 157.

- [x] Plugins: native plugins (a folder or a `.kkplugin` zip with a `plugin.json`): GLSL effects and transitions that behave like built-ins (Inspector, keyframes, undo, project files, proxies, export, Compositing page, OpenGL ES 3 rules), preset packs (recipes of commands, one undo step) in the Effects library, and python3 scripts in Workspace > Scripts (spec 157)
- [x] Plugins: Preferences > Plugins (list, install a file or a folder, drop a `.kkplugin` on the window, uninstall, enable and disable, reload, open the folder, problems with their lines); a missing plugin keeps the effect and renders as bypass (spec 157)
- [x] Plugins: example plugins: Invert, Posterize and Dip to Color as plugins that draw exactly what the built-ins draw, a two-colour effect, a circle transition, an animation pack and a script, and `plugin copy_builtin` to start from a built-in (spec 157)
- [x] Plugins: commands for the command line and MCP: list, info, create, copy_builtin, validate (errors with file and line, OpenGL 3.3 and ES 3.0), install, uninstall, enable, disable, reload, pack and render_preview (a picture of the result for an AI) (spec 157)
- [x] Plugins: documentation an AI can write plugins from: reference, installing guide, a short prompt (`llms.txt`) and the manifest JSON Schema
- [ ] Plugins: point parameters with on-screen handles, a second input and mask inputs, multi-pass effects (a puppet-pin warp, trackers) in plugin API 2
- [x] Plugins: OpenFX host for video effects: bundles from `/usr/OFX/Plugins`, `~/.OFX/Plugins`, `OFX_PLUGIN_PATH` and the Kinoklip folder, Filter and General contexts, parameters with keyframes, premultiplied float CPU rendering with render scale, plugin thread-safety flags and the multi-thread suite, missing plugin kept as bypass, a probe process and cache with a bad-plugin list and Retry, Preferences > Plugins page, openfx-misc (spec 158, [openfx.md](plugins/openfx.md))
- [x] Plugins: OpenFX Transition context (SourceFrom, SourceTo, the host-set "Transition" progress) and regions of definition larger than the clip (blurs and glows are no longer cut at the clip edge) (spec 161, [openfx.md](plugins/openfx.md))
- [ ] Plugins: OpenFX several required inputs and masks, viewer overlays, a bundled G'MIC pack (built and tested through `scripts/build_gmic_ofx.sh`, but its licences keep it out of the AppImage, spec 161.7)
- [x] Plugins: LV2 (lilv) and CLAP audio effects on the Audio page and the clip audio effects: parameters, track automation, undo, project files, export, latency compensation, a probe process with a bad-plugin list and Retry, LV2 / CLAP sections on the Plugins page, and the OpenFX, LV2 and CLAP scans run in the background, up to three probes at once (spec 159, [audio-plugins.md](plugins/audio-plugins.md))
- [ ] Plugins: LV2 / CLAP plugin windows (their own interfaces), MIDI and instrument plugins, plugin state (presets saved by the plugin), side-chain inputs of plugins, surround plugins
- [x] Plugins: a bundled pack of gl-transitions (MIT) as plugins: 394 transitions with their uniforms as parameters in Effects library > Transitions > GL Transitions (spec 161, [design.md](plugins/design.md))
- [x] Plugins: presets at an edit point (transition presets, `"target": "edit_point"`, `$left` / `$right` / `$edit`) and a Plugins folder in the Compositing node list that follows reloads (spec 161)

## Next big release

A large feature, so it gets its own big release. Smaller fix releases will come out before it.

- [ ] Media: stereo 3D: stereo footage support (clips with both eyes, a 3D viewer mode), then advanced stereo alignment for two-camera footage (vertical, rotation, scale and convergence correction, automatic from matched features, plus colour match between the eyes); builds on the tracker (user request)
- [ ] Media: raw video: decode open raw formats (CinemaDNG, ProRes RAW where FFmpeg supports it) and a simple Darktable-style panel: exposure, white balance, highlights, shadows, and highlight recovery (user request)
- [x] Tracking: easy adaptable masks on the Edit page: Follow Subject > Track in every mask group (and `mask follow`, so also the command line and the MCP server) makes a mask drawn once follow the subject under it, forward and back, with position, scale and rotation keys in one undo step (spec 187; Color page windows already have the Tracker palette, spec 107)
- [ ] Tracking: easy adaptable masks, rest: the same one-click Track for the masks of the Compositing page, tracking a Color page node without a window, perspective (3D) following, keeping the mask's own keys
- [x] Effects: Object Removal (Revival): hides a person or object that walks through a steady shot by filling the region from the frames before and after (temporal median, no AI model; spec 186; test_effects5)
- [ ] Effects: Object Removal for moving cameras and a region that follows the object by itself (a mask or tracker input), filling from a chosen clean plate frame

Rule for every AI-assisted tool: optional, clearly labelled, downloaded only when the user asks, runs locally (no cloud, no account), and only open-source code and model weights with GPL-compatible licences (for example [SAM 2](https://github.com/facebookresearch/sam2) and [LaMa](https://github.com/advimman/lama), Apache-2.0). No proprietary software or models.

## Later

Planned, not started. Compare each page with Resolve and add most of what is missing, starting
when the owner asks for it.

- [x] Color: AgX colour management, a native AgX node on the Color page and an output tone mapping in Project Settings > Color Management (user request, issue 10; spec 112; guide use-agx)
- [x] Website on GitHub Pages (starts when the owner says): home page with a Download button for the latest AppImage, the guide as searchable pages built from docs/guide, new screenshots in Breeze Dark with real (not generated) people in the sample footage, and a simple bug report form that opens a GitHub issue for people who have no GitHub account

- [x] Audio: Bounce Mix to Track and Bounce Selected Tracks (Timeline menu): render the whole mix, or each selected track with its effects, into a new audio track as one clip (spec 176; command `track bounce`, so also the command line and the MCP server)
- [x] Edit: Auto Align Clips (clip context menu): line up selected clips on different tracks by timecode or by matching their sound (user request, issue 16; spec 153; command `clip align`, so also the command line and the MCP server; guide line-up-clips-by-sound)
- [ ] Edit: Paste Mix to Current Track (Edit menu); its exact behaviour is read from public documentation first
- [ ] Edit: Trim Editor as a window of its own: outgoing and incoming frames side by side with trim buttons
- [x] Project: Archive Project (File > Project Tools > Export Project Archive / Restore Project Archive): copy the project and every media file it uses into one .dra folder, optionally with proxy media, optimized media and render cache, and restore such a folder (spec 175; commands `project archive`, `project restore-archive`)
- [ ] Project: archive only the media the timelines use; restore into the Project Manager's library (spec 175.6)
- [x] Media: metadata import and export (File > Import / Export): clip metadata of the Media Pool to and from a CSV file (spec 166)
- [x] Interface: named layouts (Workspace > Layout Presets > Save Layout as Preset / Load / Delete): keep the panels and sizes of every page under a name (spec 167)
- [ ] Interface: layout presets: window size and position, and export / import of a preset file
- [ ] Interface: translations: a language choice in Preferences and translation files that volunteers can fill in
- [x] Deliver: lossless FFV1 video (Matroska, QuickTime; preset Lossless FFV1 + FLAC) and MP3 audio as export formats (spec 165)
- [x] Platform: `kinoklip --graphics-check`: draws a test picture with the graphics card, reads it back, says whether video decodes on the card and why not, and prints a report for bug reports (spec 177)
- [ ] Platform: a Flathub submission (the Kinoklip Flatpak repository exists, see "Next version")
- [ ] Pages: compare the Media, Cut, Compositing, Color, Audio and Deliver pages with Resolve and add what is missing
- [ ] Media: clone tool on the Media page
- [x] Deliver: render presets per platform: YouTube, Vimeo, X, TikTok, Instagram, Facebook (spec 165.3)
- [ ] Titles: automatic subtitles
- [x] Color: relative LUT paths for grade node and input LUTs (spec 168.1)
- [x] Color: packing LUTs into single-file projects (spec 175.5)
- [x] Color: thumbnails of the clip itself when a higher track covers it (spec 178)
- [ ] Audio: recording and ADR, plugins, surround formats, sound library
- [ ] Audio: elastic wave, console mapping, VCAs, more effects
- [ ] Project: pack with trimming (only the used parts of long clips plus handles) and image sequences inside single-file projects
- [ ] Project: more than one project library, Open in a New Window, a cloud / shared library
- [x] Interchange: EDL export with more than one video track (spec 169.1)
- [x] Edit: type a marker Duration in the Edit Index table (spec 169.2)
- [ ] Platform: cache shaped clip names so painting a 1,500-clip timeline is cheaper
- [ ] Edit: two-up trim view with the program look (effects, transforms) without lag
- [ ] Platform: fewer dropped frames on the first play of 4K HEVC
- [ ] Edit: every remaining Resolve behaviour and quirk, page by page, driven by [behavior-spec.md](behavior-spec.md) and the parity tests
- [x] Platform: memory leak checks with ASan and LeakSanitizer (`scripts/check_leaks.sh`) and a playback soak test, run before every release
- [ ] Platform: UBSan and TSan checks

## Long-term

- [ ] Platform: Windows build, after the first stable (non-beta) release ([#4](https://github.com/SeniorMatt/kinoklip/issues/4)) (KDE Craft provides Qt, KF6 and Breeze there); the code already keeps platform code in `src/platform/`
- [ ] Platform: distro packages (Arch PKGBUILD / AUR, .deb, .rpm), Windows installer
- [ ] Interface: interface profiles beyond the default (a Plasma profile)
- [ ] Project: project website presenting Kinoklip as a free alternative to DaVinci Resolve: features, screenshots, downloads, roadmap, docs. It names Resolve only for comparison, uses no Blackmagic logos or screenshots, and claims only what the shipped build does
- [ ] Project: version 1.0.0, released only when the owner decides; until then every build is an alpha or beta

## Done

- [x] Playback: transport with J / K / L shuttle, frame stepping, looping and the sound following the output device (spec 1)
- [x] Playback: moving the playhead during playback carries playback on from the new frame (spec 48)
- [x] Playback: playhead moves, edit and marker jumps, typing a timecode (spec 2)
- [x] Playback: zoom and scrolling of the timeline, Shift+Z back to the previous zoom, follow during playback (spec 19, 20)
- [x] Edit: In and Out marks and clip marks, undoable (spec 3)
- [x] Edit: source edits Insert, Overwrite, Replace, Place on Top and Append at End (spec 4)
- [x] Edit: drag and drop to the timeline with a ghost clip, also from the file manager (spec 5)
- [x] Edit: destination controls and auto track selectors (spec 6)
- [x] Edit: selection tools, linked selection, box select (spec 7)
- [x] Edit: ripple, roll, slip, slide, Dynamic Trim and the two-up trim view (spec 8)
- [x] Edit: Blade and split at the playhead (spec 9)
- [x] Edit: delete, ripple delete and closing gaps without splitting linked clips (spec 10)
- [x] Timeline: snapping to edits, markers, keyframes and guides (spec 11)
- [x] Timeline: track headers with lock, mute, solo, auto select, rename, add and delete track (spec 12)
- [x] Timeline: moving clips between tracks with overwrite and ripple behaviour (spec 21)
- [x] Edit: copy, cut, paste and paste attributes (spec 16)
- [x] Edit: undo and redo of every step, across timelines (spec 17)
- [x] Edit: Edit Index table of the timeline's events (spec 18)
- [x] Edit: coloured markers and flags with notes, durations and navigation (spec 18)
- [x] Timeline: several timelines in tabs; create, duplicate and delete timelines (spec 25)
- [x] Timeline: compound clips and nested timelines (spec 26)
- [x] Timeline: animations of timeline edits, switchable in Preferences (spec 36)
- [x] Timeline: audio level lines and fade handles on audio clips (spec 31)
- [x] Timeline: stacked timelines (spec 31)
- [x] Timeline: clips of offline media drawn red as Media Offline (spec 12)
- [x] Edit: Transform, Cropping, Dynamic Zoom, Composite Mode with 19 blend modes, Audio volume and pan (spec 15, 24; v1.1.0 notes)
- [x] Edit: per-section keyframes with a Keyframes panel, dope sheet and curve editor (spec 15, 49)
- [x] Edit: constant speed, reverse, freeze frames and speed ramps with a retime curve (spec 29)
- [x] Edit: Clip Attributes and input interpretation (spec 31)
- [x] Compositing: rectangle, ellipse, polygon, bezier and freehand masks with feathering and modes (spec 27)
- [x] Edit: adjustment clips that apply effects to every track below (spec 28)
- [x] Titles: text titles, lower thirds, scrolling credits, text box and title presets (spec 14; v1.1.1 notes)
- [x] Titles: subtitle tracks with captions, styles and import of subtitle files in several encodings (spec 28; release notes)
- [x] Edit: still images and generators on the timeline (spec 14)
- [x] Effects: Cross Dissolve, Dip to Color, Wipe and Push on cuts, with duration handling (spec 13)
- [x] Effects: audio crossfades Cross Fade 0 dB, +3 dB, -3 dB, Exponential, Logarithmic and S-Curve (spec 13)
- [x] Effects: video effects in library folders with Live Preview on hover (spec 40, 47)
- [x] Effects: Chroma Key with eyedropper, Luma Key and HSL Key (spec 40)
- [x] Effects: blur, sharpen, stylize, distort, light and time effects with keyframes (spec 40; guide pages)
- [x] Effects: Apply LUT effect from a .cube file (spec 40, 44)
- [x] Effects: Region Blur on an adjustment clip (release notes 1.1.x)
- [x] Media: media pool with bins as folders, sorting by type, thumbnail size slider and Back / Forward mouse buttons (spec 33; v1.1.x notes)
- [x] Media: smart bins and power bins shared by every project (spec 33)
- [x] Media: clip colours and flags in the media pool and on the timeline (spec 33)
- [x] Media: proxy media, optimized media and background generation with progress badges (spec 30)
- [x] Media: render cache with background caching (spec 30)
- [x] Media: phone clips with rotation import upright, HDR phone video (HLG, PQ) looks right on an SDR timeline (spec 46)
- [x] Media: clear messages for empty, unreadable or incomplete files (spec 46)
- [x] Media: scanning and probing run in the background (spec 33)
- [x] Edit: multicam clips created from selected clips and synced by sound (spec 32)
- [x] Pages: dual timeline, Source Clip, Source Tape and Timeline viewer modes (spec 35)
- [x] Pages: Smart Insert, Append, Ripple Overwrite, Place On Top, Close Up and Source Overwrite (spec 35)
- [x] Audio: Audio page with mixer strips, buses, 6-band EQ per track and bus (spec 37)
- [x] Audio: compressor per track and bus, mute, solo, mono and stereo channel formats (spec 37)
- [x] Audio: fader and pan automation (Read, Off, Touch) (spec 37)
- [x] Audio: peak meters and BS.1770 loudness meter (momentary, short-term, integrated) (spec 37)
- [x] Audio: Normalize Audio Levels with sample peak, true peak and LUFS targets (spec 37)
- [x] Audio: audio scrubbing while dragging the playhead (spec 23, 37)
- [x] Audio: waveforms that follow clip volume and show loudness (release notes 1.1.x)
- [x] Color: node-based grades per clip and per timeline, with serial, parallel and layer nodes (spec 44; color-page.md)
- [x] Color: primaries wheels and bars with Lift, Gamma, Gain, Offset, contrast, saturation, hue, temperature and tint (spec 44)
- [x] Color: custom curves and the Hue vs Hue, Hue vs Sat, Hue vs Lum, Lum vs Sat and Sat vs Sat curves (spec 44)
- [x] Color: HSL qualifier with highlight (spec 44)
- [x] Color: power windows Circle, Linear, Polygon and Gradient with on-screen handles (spec 44)
- [x] Color: gallery stills, applying a grade to other clips and the split wipe (spec 44; v1.1.0 notes)
- [x] Color: scopes (waveform, parade, vectorscope, histogram), also as a floating window on any page (spec 44; v1.1.0 notes)
- [x] Color: LUT nodes and Apply LUT (spec 44; guide use-a-lut)
- [x] Compositing: node graph on any clip with Merge, Dissolve, Transform, Crop, Resize, Letterbox, Background, Text, masks, Color Corrector and Channel Booleans (spec 42; compositing-page.md)
- [x] Compositing: two viewers with on-screen handles and a node editor with undo (spec 42)
- [x] Compositing: Fast Noise, Polygon, Ellipse and Rectangle nodes and every Edit page effect as a node (spec 42; v1.0.0 notes)
- [x] Deliver: render queue with editable jobs (spec 45)
- [x] Deliver: H.264, H.265, AV1, ProRes, DNxHR, VP9, image sequences and audio-only formats, with hardware encoding where available (v0.1.0 notes; spec 30, 45)
- [x] Deliver: render range In/Out or entire timeline and subtitle export (spec 45)
- [x] Deliver: Render complete notification with Open Folder and Play (v1.1.1 notes)
- [x] Deliver: resolution presets in Timeline and Project Settings (v1.1.0 notes)
- [x] Project: Project Manager with a project library, crash recovery and autosave (spec 38)
- [x] Project: single-file projects (.kinoklip) that play media in place (spec 39; single-file-project.md)
- [x] Project: Create New Timeline dialog and a frame rate question on first import (v1.1.0 notes)
- [x] Interchange: EDL export of the timeline and marker export to EDL and CSV (spec 18)
- [x] Interface: viewer guides and rulers (spec 41)
- [x] Interface: colour schemes that every painted part follows (spec 34)
- [x] Interface: in-app help, F1 opens the guide article for what the pointer is on (spec 43)
- [x] Interface: how-to guides, shown in the app's help (docs/guide)
- [x] Interface: Keyboard Customization with presets of other editors and import of their keyboard files (spec 23; keymaps)
- [x] Interface: panel sizes remembered, Reset UI Layout (v1.1.x notes)
- [x] Interface: menu bar with File > Import, Workspace and the grouped View menu (spec 23)
- [x] Interface: Report a Bug or Inconsistency from the Help menu (v1.1.x notes)
- [x] Interface: Menu layout option, Resolve or Streamlined (shorter menus grouped by task, a page menu, shorter context menus) (spec 82)
- [x] Platform: single AppImage for x86_64 Linux on X11 and Wayland (release notes)
- [x] Platform: privacy by design, no telemetry or automatic crash reports (privacy.md)

## Rename

- Rename Kinoklip to **Wolfgang Montage** (after Johann Wolfgang von Goethe, who also wrote a theory of colours). Not started: it happens only when the owner says so. It covers the app name, the repository, the AppImage and desktop files, settings and project paths (with a migration for existing users), the docs and the website
- New logo, wordmark and icon for the new name, drawn by hand by a human artist (never AI-generated). The rename waits for them

## Not planned

- In-app update check or automatic updates: Kinoklip sends nothing and fetches nothing by itself; new versions are on the releases page (owner decision)
- Final Cut Pro 7 / Premiere XML import and export (owner decision)
- macOS version (OpenGL is frozen there, a port would need a Metal renderer, signing and a Mac to test on)
- AAF and FCPXML interchange (owner, 2026-11-11): OpenTimelineIO already moves timelines to Resolve, and its adapters can convert to both; brought back if a user asks.
- Importing Resolve project files: they are undocumented and could only be read by analysing them (clean room). Moving work over stays possible through open formats (EDL today; see AAF and FCPXML under Later)
