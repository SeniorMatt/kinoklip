# Kinoklip roadmap

What is planned, in order. Nothing here is a promise: the owner decides what starts and when a
version is released. Finished milestones (v1 to v4) and the old plans are in [archive/](archive/).
Bugs and ideas from users: [GitHub issues](https://github.com/SeniorMatt/kinoklip/issues).

Rules for all work: pages and features get their own names (no Blackmagic product names), and
everything follows the [clean-room policy](clean-room.md).

## Version 1.2 (starts right after 1.1.8)

Everything below in "Now" is planned for 1.2, in this order: [OpenTimelineIO](https://opentimeline.io), planar tracker, audio effects, the command-line interface and the MCP server on top of it, then the missing transitions, effects and generators (Smooth Cut first), stabilization, fade curves, hover preview, the text title layers, emoji, Inspector audio options, settings import/export and vim keys. 1.1.8 has the small fixes (search box, Keyframes / Curves switch, licence texts, FUSE note).

## Now (next version)

Shipped in 1.1.7: thumbnails give way to playback ([#2](https://github.com/SeniorMatt/kinoklip/issues/2)), the point tracker ([#3](https://github.com/SeniorMatt/kinoklip/issues/3)), Close Gap and Ripple Delete on gaps. Waiting for the reporter to confirm.

User requests come first: they are the most valuable tasks.

From users (high priority)
- OpenTimelineIO (.otio) import and export, to move timelines to and from Resolve and other editors (OpenTimelineIO C++ library, Apache-2.0) ([#5](https://github.com/SeniorMatt/kinoklip/issues/5))
- Compositing: planar tracker, and linking other nodes' positions to a track (the point tracker shipped in 1.1.7, [#3](https://github.com/SeniorMatt/kinoklip/issues/3))

Bugs and fixes
- Media Pool: the thumbnail slider squeezes the search box at the default width
- Keyframes panel: the Keyframes / Curves switch should look like native Breeze / [Qt](https://www.qt.io)
- AppImage: show an error when FUSE is missing (today it fails silently)
- Bundle: complete licence texts for every bundled library (xz is empty)

Editing
- Media Pool: hovering a clip thumbnail previews it in the viewer, as in Resolve
- Fade curves on timeline clips that can be shaped with the mouse (the handles draw a straight line today)
- Effects library: every video transition, video effect and generator that Resolve has and Kinoklip lacks, Smooth Cut first (owner request)
- Stabilization
- Emoji in subtitles and titles, in colour, in the viewer and in exports
- Text title: more options on the existing title, mainly layers (several text and shape layers in one title); no new title type

Audio
- Audio effects on audio clips (Add Effect is disabled in Inspector > Effects)
- Inspector Audio tab: compare with every audio option Resolve has (public documentation only) and add the missing ones, Pitch first

Tools and settings
- Import and export all settings with one click
- Vim keybindings, so nvim users can edit easily
- Command-line interface
- MCP server

## Next big release

A large feature, so it gets its own big release. Smaller fix releases will come out before it.

- Stereo 3D: stereo footage support (clips with both eyes, a 3D viewer mode), then advanced stereo alignment for two-camera footage (vertical, rotation, scale and convergence correction, automatic from matched features, plus colour match between the eyes); builds on the tracker (user request)
- Raw video: decode open raw formats (CinemaDNG, ProRes RAW where [FFmpeg](https://ffmpeg.org) supports it) and a simple Darktable-style panel: exposure, white balance, highlights, shadows, and highlight recovery (user request)
- Easy adaptable masks: a mask drawn once follows the subject by itself (tracked windows and shapes, built on the tracker) (user request)
- Rule for every AI-assisted tool: optional, clearly labelled, downloaded only when the user asks, runs locally (no cloud, no account), and only open-source code and model weights with GPL-compatible licences (for example [SAM 2](https://github.com/facebookresearch/sam2) and [LaMa](https://github.com/advimman/lama), Apache-2.0). No proprietary software or models
- Local replacement: hide people or objects that walk into a shot by filling the masked area from other frames (clean plate), steady shots first (user request)

## Later

Planned, not started. Compare each page with Resolve and add most of what is missing, starting
when the owner asks for it.

Compositing
- Mask tracking on the Edit page
- Paint (B-spline and paint tools), particles, 3D, expressions, modifiers

Pages
- Compare the Media, Cut, Compositing, Color, Audio and Deliver pages with Resolve and add what is missing
- Media page: clone tool
- Deliver page: render presets per platform
- Automatic subtitles

Color page
- Tracking of windows, polygon shape keyframes, Bezier windows
- Midtone Detail, soft clip, HDR and log wheels
- Key mixer, splitter and outside nodes; node presets and versions; ColorTrace and grade groups
- Keyframe strip, Sizing palette, node labels typed in the editor
- Qualifier 3D / RGB / Luma modes, lightbox, split-screen modes other than the split wipe
- Relative LUT paths, and packing LUTs into single-file projects
- Thumbnails of the clip itself when a higher track covers it

Audio page
- Recording and ADR, plugins, surround formats, sound library
- Dialogue leveler, elastic wave, console mapping, VCAs, more effects

Projects and interchange
- Pack with trimming (only the used parts of long clips plus handles) and image sequences inside single-file projects
- More than one project library, Open in a New Window, a cloud / shared library
- AAF and FCPXML interchange, EDL export with more than one video track

Edit page polish
- Type a marker Duration in the Edit Index table
- Cache shaped clip names so painting a 1,500-clip timeline is cheaper
- Two-up trim view with the program look (effects, transforms) without lag
- Fewer dropped frames on the first play of 4K HEVC
- Every remaining Resolve behaviour and quirk, page by page, driven by [behavior-spec.md](behavior-spec.md) and the parity tests

Quality
- Memory checks with ASan, UBSan and TSan (declined for now)

## Rename

- Rename Kinoklip to **Wolfgang Montage** (after Johann Wolfgang von Goethe, who also wrote a theory of colours). Not started: it happens only when the owner says so. It covers the app name, the repository, the AppImage and desktop files, settings and project paths (with a migration for existing users), the docs and the website
- New logo, wordmark and icon for the new name, drawn by hand by a human artist (never AI-generated). The rename waits for them

## Long-term

- Windows build, after the first stable (non-beta) release ([#4](https://github.com/SeniorMatt/kinoklip/issues/4)) (KDE Craft provides Qt, KF6 and Breeze there); the code already keeps platform code in `src/platform/`
- [Flatpak](https://flatpak.org) and distro packages (Arch PKGBUILD / AUR, .deb, .rpm), Windows installer
- Interface profiles beyond the default (a Plasma profile)
- Project website presenting Kinoklip as a free alternative to [DaVinci Resolve](https://www.blackmagicdesign.com/products/davinciresolve): features, screenshots, downloads, roadmap, docs. It names Resolve only for comparison, uses no Blackmagic logos or screenshots, and claims only what the shipped build does
- Version 1.0.0: released only when the owner decides; until then every build is an alpha or beta

## Not planned

- macOS version (OpenGL is frozen there, a port would need a Metal renderer, signing and a Mac to test on)
- Background removal
- Importing Resolve project files: they are undocumented and could only be read by analysing them (clean room). Moving work over stays possible through open formats (EDL today; see AAF and FCPXML under Later)
