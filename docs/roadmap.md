# Kinoklip roadmap

What is planned, in order. Nothing here is a promise: the owner decides what starts and when a
version is released. Finished milestones (v1 to v4) and the old plans are in [archive/](archive/).
Bugs and ideas from users: [GitHub issues](https://github.com/SeniorMatt/kinoklip/issues).

Rules for all work: pages and features get their own names (no Blackmagic product names), and
everything follows the [clean-room policy](clean-room.md).

## Now (next version)

Planned on 2026-11-05 for the version after 1.1.5. Not started.

User requests come first: they are the most valuable tasks.

From users (high priority)
- Playback drops to a few FPS while timeline thumbnails regenerate; thumbnails should give way to playback ([#2](https://github.com/SeniorMatt/kinoklip/issues/2))
- Compositing: basic tracking (point tracker first, then planar), the last missing piece for editing a full video for some users ([#3](https://github.com/SeniorMatt/kinoklip/issues/3))

Bugs and fixes
- Timeline: Close Gap in the context menu does nothing on a gap after the last clip
- Timeline: Ripple Delete must work on empty space (a selected gap)
- Media Pool: the thumbnail slider squeezes the search box at the default width
- Keyframes panel: the Keyframes / Curves switch should look like native Breeze / Qt
- AppImage: show an error when FUSE is missing (today it fails silently)
- Bundle: complete licence texts for every bundled library (xz is empty)

Editing
- Media Pool: hovering a clip thumbnail previews it in the viewer, as in Resolve
- Fade curves on timeline clips that can be shaped with the mouse (the handles draw a straight line today)
- Smooth Cut transition in the Effects library
- Stabilization
- Emoji in subtitles and titles, in colour, in the viewer and in exports
- One text title that does everything Text+ does; no separate Text+ type

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

## Long-term

- Windows and macOS builds (KDE Craft provides Qt, KF6 and Breeze there); the code already keeps platform code in `src/platform/`
- Flatpak and distro packages (Arch PKGBUILD / AUR, .deb, .rpm), Windows installer, macOS .dmg
- Interface profiles beyond the default (a Plasma profile)
- Project website presenting Kinoklip as a free alternative to DaVinci Resolve: features, screenshots, downloads, roadmap, docs. It names Resolve only for comparison, uses no Blackmagic logos or screenshots, and claims only what the shipped build does
- Version 1.0.0: released only when the owner decides; until then every build is an alpha or beta

## Not planned

- Background removal
- Importing Resolve project files: they are undocumented and could only be read by analysing them (clean room). Moving work over stays possible through open formats (EDL today; see AAF and FCPXML under Later)
