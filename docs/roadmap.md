# Kinoklip roadmap

What is planned, in order. Nothing here is a promise: the owner decides what starts and when a
version is released. Finished milestones (v1 to v4) and the old plans are in [archive/](archive/).
Bugs and ideas from users: [GitHub issues](https://github.com/SeniorMatt/kinoklip/issues).

Rules for all work: pages and features get their own names (no Blackmagic product names), and
everything follows the [clean-room policy](clean-room.md).

Every feature is one checklist line, `- [x]` when made and `- [ ]` when not, with its area before the first colon. `python3 scripts/status.py` counts the lines (and the table in [effects-library-status.md](effects-library-status.md)) and draws the picture in the readme.

## Version 1.2 (starts right after 1.1.8)

Everything below in "Now" is planned for 1.2, in this order: [OpenTimelineIO](https://opentimeline.io), planar tracker, audio effects, the command-line interface and the MCP server on top of it, then the missing transitions, effects and generators (Smooth Cut first), stabilization, fade curves, hover preview, the text title layers, emoji, Inspector audio options, settings import/export and vim keys. 1.1.8 has the small fixes (search box, Keyframes / Curves switch, licence texts, FUSE note).

## Now (next version)

User requests come first: they are the most valuable tasks.

Shipped in 1.1.7:
- [x] Playback: thumbnails give way to playback ([#2](https://github.com/SeniorMatt/kinoklip/issues/2))
- [x] Tracking: the point tracker ([#3](https://github.com/SeniorMatt/kinoklip/issues/3))
- [x] Edit: Close Gap and Ripple Delete on gaps

Shipped in 1.1.8:
- [x] Media: the thumbnail slider no longer squeezes the search box at the default width
- [x] Interface: the Keyframes / Curves switch looks like native Breeze / [Qt](https://www.qt.io)
- [x] Platform: complete licence texts for every bundled library

Done for 1.2 so far (on main, not released yet):
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
- [ ] Tracking: Match Move
- [ ] Tracking: Surface Tracker
- [ ] Effects: Warper
- [x] Effects: CCTV look
- [x] Effects: Digital Glitch look
- [x] Effects: Drone Overlay look
- [x] Effects: DSLR look
- [x] Effects: DVE look
- [x] Effects: Video Call look
- [x] Effects: Video Camera look
- [ ] Titles: more options on the existing text title, mainly layers (several text and shape layers in one title); no new title type
- [ ] Titles: emoji in subtitles and titles, in colour, in the viewer and in exports
- [x] Audio: Inspector Audio tab compared with every audio option Resolve has (public documentation only), adding the missing ones, Pitch first (spec 67: Pitch and a four-band clip Equalizer added; Voice Isolation and Dialogue Leveler skipped, they need a model)
- [x] Interface: import and export all settings with one click
- [x] Interface: vim keybindings, so nvim users can edit easily
- [x] Interface: Help > Export Log File: saves what the app did in this session (messages, warnings, the decoder chosen for each file, the graphics card, the playback statistics) as one text file to attach to a bug report. The log is kept in memory while you work and written only when you ask; the dialog says what the file contains (owner pick, idea from [Picasso Suite](https://github.com/fabox-os/Picasso-Suite))

## Next big release

A large feature, so it gets its own big release. Smaller fix releases will come out before it.

- [ ] Media: stereo 3D: stereo footage support (clips with both eyes, a 3D viewer mode), then advanced stereo alignment for two-camera footage (vertical, rotation, scale and convergence correction, automatic from matched features, plus colour match between the eyes); builds on the tracker (user request)
- [ ] Media: raw video: decode open raw formats (CinemaDNG, ProRes RAW where [FFmpeg](https://ffmpeg.org) supports it) and a simple Darktable-style panel: exposure, white balance, highlights, shadows, and highlight recovery (user request)
- [ ] Tracking: easy adaptable masks: a mask drawn once follows the subject by itself (tracked windows and shapes, built on the tracker) (user request)
- [ ] Effects: hide people or objects that walk into a shot by filling the masked area from other frames (clean plate), steady shots first (user request)

Rule for every AI-assisted tool: optional, clearly labelled, downloaded only when the user asks, runs locally (no cloud, no account), and only open-source code and model weights with GPL-compatible licences (for example [SAM 2](https://github.com/facebookresearch/sam2) and [LaMa](https://github.com/advimman/lama), Apache-2.0). No proprietary software or models.

## Later

Planned, not started. Compare each page with Resolve and add most of what is missing, starting
when the owner asks for it.

- [ ] Audio: voice-over: record from the microphone straight onto an audio track while the timeline plays. An arm button and an input menu on the track header (Edit and Audio pages), an input level meter, a count-in, and each take saved as a WAV file beside the project and added to the Media Pool (owner pick)
- [ ] Audio: Bounce Mix to Track and Bounce Selected Tracks (Timeline menu): render the whole mix, or each selected track with its effects, into a new audio track as one clip
- [ ] Edit: Auto Align Clips (clip context menu): line up selected clips on different tracks by timecode or by matching their sound
- [ ] Edit: Paste Mix to Current Track (Edit menu); its exact behaviour is read from public documentation first
- [ ] Edit: Trim Editor as a window of its own: outgoing and incoming frames side by side with trim buttons
- [ ] Project: Archive Project (File menu): copy the project and every media file it uses into one folder, and open such a folder again
- [ ] Media: metadata import and export (File > Import / Export): clip metadata of the Media Pool to and from a CSV file
- [ ] Interface: named layouts (Workspace > Layout > Save Layout / Load Layout): keep the panel sizes of every page under a name
- [ ] Interface: translations: a language choice in Preferences and translation files that volunteers can fill in
- [ ] Deliver: lossless FFV1 video and MP3 audio as export formats
- [ ] Platform: `kinoklip --graphics-check`: draws a test picture with the graphics card, reads it back, says whether video decodes on the card and why not, and prints a report for bug reports
- [ ] Platform: AppImage for ARM64 computers, and a Flathub submission once the Flatpak exists
- [ ] Compositing: mask tracking on the Edit page
- [ ] Compositing: Paint (B-spline and paint tools), particles, 3D, expressions, modifiers
- [ ] Pages: compare the Media, Cut, Compositing, Color, Audio and Deliver pages with Resolve and add what is missing
- [ ] Media: clone tool on the Media page
- [ ] Deliver: render presets per platform
- [ ] Titles: automatic subtitles
- [ ] Color: tracking of windows, polygon shape keyframes, Bezier windows
- [ ] Color: Midtone Detail, soft clip, HDR and log wheels
- [ ] Color: key mixer, splitter and outside nodes; node presets and versions; ColorTrace and grade groups
- [ ] Color: keyframe strip, Sizing palette, node labels typed in the editor
- [ ] Color: Qualifier 3D / RGB / Luma modes, lightbox, split-screen modes other than the split wipe
- [ ] Color: relative LUT paths, and packing LUTs into single-file projects
- [ ] Color: thumbnails of the clip itself when a higher track covers it
- [ ] Audio: recording and ADR, plugins, surround formats, sound library
- [ ] Audio: dialogue leveler, elastic wave, console mapping, VCAs, more effects
- [ ] Project: pack with trimming (only the used parts of long clips plus handles) and image sequences inside single-file projects
- [ ] Project: more than one project library, Open in a New Window, a cloud / shared library
- [ ] Interchange: AAF and FCPXML interchange, EDL export with more than one video track
- [ ] Edit: type a marker Duration in the Edit Index table
- [ ] Platform: cache shaped clip names so painting a 1,500-clip timeline is cheaper
- [ ] Edit: two-up trim view with the program look (effects, transforms) without lag
- [ ] Platform: fewer dropped frames on the first play of 4K HEVC
- [ ] Edit: every remaining Resolve behaviour and quirk, page by page, driven by [behavior-spec.md](behavior-spec.md) and the parity tests
- [ ] Platform: memory checks with ASan, UBSan and TSan (declined for now)

## Long-term

- [ ] Platform: Windows build, after the first stable (non-beta) release ([#4](https://github.com/SeniorMatt/kinoklip/issues/4)) (KDE Craft provides Qt, KF6 and Breeze there); the code already keeps platform code in `src/platform/`
- [ ] Platform: [Flatpak](https://flatpak.org) and distro packages (Arch PKGBUILD / AUR, .deb, .rpm), Windows installer
- [ ] Interface: interface profiles beyond the default (a Plasma profile)
- [ ] Project: project website presenting Kinoklip as a free alternative to [DaVinci Resolve](https://www.blackmagicdesign.com/products/davinciresolve): features, screenshots, downloads, roadmap, docs. It names Resolve only for comparison, uses no Blackmagic logos or screenshots, and claims only what the shipped build does
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
- [x] Platform: single AppImage for x86_64 Linux on X11 and Wayland (release notes)
- [x] Platform: privacy by design, no telemetry or automatic crash reports (privacy.md)

## Rename

- Rename Kinoklip to **Wolfgang Montage** (after Johann Wolfgang von Goethe, who also wrote a theory of colours). Not started: it happens only when the owner says so. It covers the app name, the repository, the AppImage and desktop files, settings and project paths (with a migration for existing users), the docs and the website
- New logo, wordmark and icon for the new name, drawn by hand by a human artist (never AI-generated). The rename waits for them

## Not planned

- In-app update check or automatic updates: Kinoklip sends nothing and fetches nothing by itself; new versions are on the releases page (owner decision)
- Final Cut Pro 7 / Premiere XML import and export (owner decision)
- macOS version (OpenGL is frozen there, a port would need a Metal renderer, signing and a Mac to test on)
- Background removal
- Importing Resolve project files: they are undocumented and could only be read by analysing them (clean room). Moving work over stays possible through open formats (EDL today; see AAF and FCPXML under Later)
