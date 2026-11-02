# Polish v3: basic editing on the Edit page

Roadmap v3 (docs/roadmap.md): make everyday editing feel finished. This page is the audit of
the Edit page against a normal Resolve editing session (import, browse, mark, insert /
overwrite, arrange, trim, blade, transitions, titles, audio levels, keyframes, render), and
the status of each item on the `editpolish` branch. Behaviour is decided from general
knowledge of Resolve (clean room); decisions are recorded in docs/behavior-spec.md.

Priorities: **P1** = noticed in every session, or a deferred spec item; **P2** = noticed
often; **P3** = nice to have / rare.
Round 1 was the `editpolish` branch, round 2 the `polish2` branch (section "Round 2" below).

Status: **Done** (on this branch, with tests), **OK** (already fine, verified), **Open**
(not done in this round), **Partial**. Tests: `ea` = tests/timeline/test_editaids.cpp, `vs` =
tests/playback/test_varispeed.cpp, test_editpolish = tests/app/test_editpolish.cpp (the real
main window; also writes the screenshots when KK_SCREENSHOT_DIR is set).

## Owner feedback check (docs/feedback/alpha1-3.md)

Every item of the three feedback files was traced to its fix in the spec, status.md or
the code:

| Feedback | Where it was fixed | Status |
|---|---|---|
| alpha1 #1 scheme switching crash / portal app id | themeswitch tests, launcher | OK |
| alpha1 #2 arrows over text, paddings | MenuMetricsStyle, dialogs tests | OK |
| alpha1 #3 Resolve layout and behaviour | ongoing (this round) | OK (ongoing) |
| alpha1 #4 ripple cut / ripple delete | spec 10.2, 10.9 | OK |
| alpha1 #5 keyframe editor | spec 15.8 - 15.20 | OK |
| alpha1 #6 wheel zoom around the playhead (setting) | spec 19.6 | OK |
| alpha1 #7 render / project settings | Project Settings dialog, Deliver page | OK |
| alpha1 #8 Resolve Windows keys, profiles | keymap script, spec 23.6 | OK |
| alpha1 #9 render panel layout | Deliver page | OK |
| alpha2 #1 KDE file dialogs | test_filedialogs | OK |
| alpha2 #2 pixelated playback | playback quality (PlaybackResolution, Auto) | OK |
| alpha2 #3 moving the playhead while playing lags | seekWhilePlaying (park) | OK |
| alpha2 #4 / #9 Ctrl+Shift+[ / ] ripple, Shift+[ / ] gap | spec 8.10, test_editkeys | OK |
| alpha2 #5 leave dual viewer | spec 24.4 | OK |
| alpha2 #6 transitions clickable | spec 13.10 | OK |
| alpha2 #10 Keyframes panel | spec 15.14 - 15.20 | OK |
| alpha2 #11 linked pair = one inspector item | spec 24.1 | OK |
| alpha3 #1 drag and drop into the timeline (Wayland) | wayland branch, test_wayland | OK |
| alpha3 #2 detaching panels not discoverable | Keyframes panel: labelled float button | OK |
| alpha3 #3 log noise, VAAPI sync failures, stats lines | hwdecode branch, docs/hw-decode-test.md | OK |
| alpha3 #5 resize flicker on Plasma Wayland | wayland branch, test_resizebench | OK |
| alpha3 #6 scheme-driven colours | spec 34 | OK |

## Audit

### Viewer and transport

| # | Item | Pri | Status |
|---|---|---|---|
| V1 | Audio while shuttling (J / L at 2x-16x), in reverse and in slow play (K+J / K+L), pitched like a tape (spec 1.16, deferred). | P1 | Done (spec 1.16; vs) |
| V2 | Audio scrubbing (Shift+S, on by default in Resolve): dragging the playhead or stepping frames plays a short snippet (spec 23.7, disabled). | P1 | Done (spec 23.7; vs) |
| V3 | Typed timecode entry: with the timeline or the timeline viewer focused, typing digits enters a timecode (right-aligned like a calculator, `.` = `00`), Enter moves the playhead there; a leading `+` / `-` makes it an offset; with clips selected an offset moves the clips. | P1 | Done (spec 2.10; ea, test_editpolish) |
| V4 | Viewer edit overlay: dragging a clip (media pool, source viewer) onto the timeline viewer shows the edit types (insert, overwrite, replace, place on top, append at end, ripple overwrite, fit to fill); dropping on one performs it (spec 4.14, deferred). | P1 | Done (spec 4.14; ea, test_editpolish) |
| V5 | Two-up trim view: while trimming / rolling / slipping / sliding the timeline viewer shows the frames on both sides of the edit, with their timecodes (spec 8.15, deferred). | P1 | Done (spec 8.15; ea, test_editpolish) |
| V6 | J/K/L, frame stepping, K+J / K+L, ruler and scrub bar scrubbing. | - | OK (spec 1.x, 2.1; pe, tw) |
| V7 | Source viewer with in / out, drag to the timeline, double-click in the media pool opens the source viewer. | - | OK (spec 3.1, 3.9, 5.1, 24.5) |
| V8 | Match frame (F), replace edit (F11). | - | OK (spec 23.1, 4.7) |
| V9 | Replace edit lines up the source viewer's playhead with the timeline playhead when the source has no in mark (spec 4.7 Decision). | P2 | Done (spec 4.7; ea) |

### Timeline

| # | Item | Pri | Status |
|---|---|---|---|
| T1 | Drag / trim info: while moving, trimming, rolling, slipping or sliding, a label next to the pointer shows the offset (`+00:00:01:05`) and the new timecode / duration. | P1 | Done (spec 21.9; ea) |
| T2 | Alt+Up / Alt+Down move the selected clips one track up / down (linked clips of the other kind stay). | P2 | Done (spec 21.10; ea; key unsure) |
| T3 | Clip context menu: Change Clip Duration (Ctrl+D) is missing (Change Clip Speed is there). | P2 | Done (spec 12.8) |
| T4 | Track header context menu: Delete Empty Tracks (Resolve's "Delete Empty Tracks"). | P2 | Done (spec 12.8; ea) |
| T5 | Track height presets in the header menu (small / medium / large for the track). | P3 | Done in round 2 (spec 12.10; p2) |
| T6 | Up / Down edit points, auto-select tracks, V / Shift+V / U, trim tool slip / slide, linked selection, snapping, gap selection and Delete Gaps, add / delete / rename tracks. | - | OK (spec 2, 6, 7, 8, 10, 11, 12) |
| T7 | Speed-change nudges (spec 29.16): Resolve has no default keys for them; commands without keys. | P3 | Open |
| T8 | Move Track Up / Down in the header menu. | P3 | Done in round 2 (spec 12.9; p2) |

### Inspector, media pool, responsiveness

| # | Item | Pri | Status |
|---|---|---|---|
| I1 | Reset buttons per parameter, scrubbable number fields (drag), typed entry (double-click), keyboard keys. | - | OK (ParamRows, ValueField; in) |
| I2 | Edits applying to every selected clip of the same kind (Resolve multi-clip inspector, spec 24.1 Decision). | P3 | Done in round 2 (spec 24.1; in) |
| M1 | Media pool drag and drop to the timeline and to the viewers; Files from a file manager. | - | OK (spec 5, test_wayland) |
| R1 | Thumbnails and waveforms appear asynchronously (MediaCache), paint never blocks. | - | OK (TimelineMedia) |
| R2 | First play after import: decoders open on demand; measure the first-play delay on the owner's machine (test_playbackperf covers steady state only). | P3 | Done in round 2 (spec 1.18; test_firstplay, numbers below) |

## Shortcuts changed on this branch

| Command | Key | Confidence |
|---|---|---|
| Audio Scrubbing (`timeline.audio_scrubbing`) | Shift+S (was disabled, now works) | sure |
| Move Clips Up / Down a Track (`trim.move_up` / `trim.move_down`) | Alt+Up / Alt+Down (new) | unsure |
| Delete Empty Tracks (`timeline.delete_empty_tracks`) | none (new menu command) | likely |

Typed timecodes are typed text (digits, + / -, '.', Enter, Esc), not commands, so they are not
in the keymap.

## Left for the next round (after round 1)

- T5 / T8, I2, R2 and the shuttle sound quality were done in round 2 (below).
- T7: speed-change nudges (no Resolve default keys known; commands only).
- The two-up view shows source frames; a program-rendered two-up (with effects) would need a
  second still request path in the engine.

## Round 2: what a Resolve user notices in the first 30 minutes

Branch `polish2` (after 0.3.0-alpha.1). First the round 1 leftovers, then a new pass over
the first half hour of a Resolve user: keyboard focus, context menus, undo names, zoom,
clip display, markers, titles, render, project management and error handling. Tests: `p2` =
tests/timeline/test_polish2.cpp, `vp` = test_viewerpanel, `ap` = test_audiopage, `cp` =
test_cutpage, `in` = test_inspector, `vs` = test_varispeed, `pe` = test_playbackengine.

### Round 1 leftovers

| # | Item | Pri | Status |
|---|---|---|---|
| L1 | Track height presets (header menu, and Track Height (All Tracks) in a new Timeline View Options button, which Resolve's timeline also has). | P3 | Done (spec 12.10, 12.11; p2) |
| L2 | Move Track Up / Down in the header menu. | P3 | Done (spec 12.9; p2) |
| L3 | Multi-clip Inspector edits: one change to every selected clip of the same kind, one undo step, "Mixed" for different values. | P2 | Done (spec 24.1 Decision; in) |
| L4 | Shuttle audio at 4x - 8x: windowed-sinc resampler with a speed-following cutoff instead of linear interpolation; real-time safe, no allocations per chunk. | P2 | Done (spec 1.16; vs spec1_16_shuttleDoesNotAlias) |
| L5 | Edit overlay on the Cut page viewer: Resolve's Cut page offers its own edits when media is dragged onto the viewer, so the overlay shows the six smart edits there. | P2 | Done (spec 35.26; cp) |
| L6 | Audio page track headers cut the name to "A...": the buttons go to a second line in tall headers. | P2 | Done (spec 37.34; ap) |
| L7 | First-play delay after import: measured, and the start of the first play fixed (numbers below). | P2 | Done (spec 1.18; test_firstplay, pe) |

### First play after import (L7)

`tests/app/test_firstplay.cpp` drives the real main window (offscreen, CPU compositor, the
owner's laptop: NVIDIA, CUDA decoding; a fake sound card that notes the first non-silent
sample). It imports a file (cold media cache), appends it to the timeline and presses Space,
either at once or after the paused frame appeared (700 ms later, as a user would). Before =
main at 0.3.0-alpha.1, after = this branch.

| Case | Before | After |
|---|---|---|
| Import (probe, GUI thread) | 4 - 7 ms | same |
| Longest GUI-thread stall, import to playing | 26 - 41 ms (play() with its 60 ms audio pre-roll wait, first paint) | 17 - 36 ms |
| h264 720p25, Space at once: first frame / first sound after play | 378 ms / 25 ms (the sound ran 350 ms ahead, 14 frames dropped) | 272 ms / 279 ms together (6 dropped) |
| h264 720p25, Space later: first frame / first sound after play | 51 ms / 11 ms, 0 dropped | same |
| screen 1080p60 / hevc 2160p30, Space at once: first frame after play | 175 / 225 ms, 9 / 11 dropped | 76 / 127 ms, 4 / 6 dropped |
| Paused frame after the drop (warm process) | 15 - 20 ms | same |

Findings: nothing heavy runs on the GUI thread (the probe is a few ms; thumbnails, waveforms
and proxies are already asynchronous). The delay was the first hardware decoder: creating the
CUDA device takes 150 - 200 ms and opening the first decoder another 250 ms, while the play
had already started the sound. Fixes: a play from rest whose start frame is not on screen
pre-rolls the first frames through the playback path and starts clock and sound with the
first one (spec 1.18); `media::warmUpHwDecoding()` opens the decoding device in the
background at start-up (saves about 150 ms on the first frame after the first import). Left:
a few frames are still dropped about 0.3 s into a first play at once (the decoder warming
up); waiting for more pre-rolled frames would delay the start further.

### Audit

| # | Item | Pri | Status |
|---|---|---|---|
| K1 | Shortcuts in every focusable window: the floating Audio page viewer and the EQ / Dynamics windows had none (Space did nothing there). | P1 | Done (spec 24.10; ap) |
| K2 | Space plays the active viewer from any panel (media pool, inspector, effects, timeline); text fields keep their keys. | - | OK (window-scoped actions, ViewerTransport; vp transportFollowsActiveViewer) |
| K3 | Escape: cancels timeline drags, keyframe drags, typed timecodes, mask drawing and the new transform drag; closes the full screen viewer. | - | OK (spec 8.13, 2.10, 27) |
| C1 | Viewer context menu: there was none. | P2 | Done (spec 24.7; vp) |
| C2 | Ruler menu on a marker: only Delete Marker. | P2 | Done: Modify Marker… (spec 18.7; p2) |
| C3 | Track header menu: no entries for the header buttons; no move / height. | P3 | Done (spec 12.9, 12.10, 12.12; p2) |
| C4 | Clip, transition, timeline empty area and media pool menus. | - | OK (spec 12.8, 13.10, 33) |
| U1 | Undo names: every header button undid as "Change Track", a multi-item drop as "Edit Clips". | P2 | Done (spec 17.5; p2); unnamed steps now log a warning |
| Z1 | Zoom: Shift+Z fit, limits, keyboard zoom keeps the playhead, Alt+wheel around the playhead (unchanged on the owner's request). Ctrl+wheel scrolled 120 px per notch on any view under 2400 px (integer step). | P3 | OK; scroll step Done (spec 19.8; p2) |
| D1 | Clip display: name and duration at small heights (the band takes the whole row); thumbnail modes and waveform toggle were missing. | P2 | Done (spec 12.11; p2) |
| D2 | Source timecode on clips. | P3 | Open (Resolve does not show it by default either) |
| M1 | Markers: M / Ctrl+M / Alt+M, colour and name editor, Shift+Up / Down, clip markers. | - | OK (spec 18) |
| M2 | Marker list in the Edit Index: the Edit Index is still a placeholder panel. | P2 | Open (needs the Edit Index panel: list of edits and markers) |
| T1 | Titles: font, size, colour in the Inspector (Title tab), one undo step per typing burst. | - | OK (spec 14.3) |
| T2 | Titles: positioning by dragging in the viewer (Resolve's on-screen Transform controls). | P1 | Done (spec 24.9; vp) |
| R1 | Deliver quick path: preset, Add to Render Queue, Render All, progress per job. | - | OK (spec 22 step 9) |
| R2 | Render finished: no message when the queue was done. | P2 | Done (spec 23.8; test_deliverpage) |
| P1 | Project management: recent projects (File > Open Recent, missing files marked), crash recovery prompt, unsaved changes prompt on quit, Project Settings as one undo step. | - | OK (ProjectController, test_session) |
| E1 | Offline media (moved file): the timeline clip and the viewer showed nothing (black), only the media pool marked it. | P1 | Done (spec 12.13, 24.8; p2, vp) |
| E2 | Relink: only the chosen folder itself was searched; no Relink Selected Clips. | P2 | Done (spec 33.20; test_robustness) |
| E3 | Unsupported or damaged files on import: one warning dialog listing each file with its reason. | - | OK (importMediaWithFeedback, test_robustness badMediaFiles) |

### Shortcuts changed in round 2

| Command | Key | Confidence |
|---|---|---|
| On-Screen Transform Controls (`view.onscreen_transform`) | none (new command; the viewer's on-screen controls menu) | unsure |

### Left after round 2

- M2: the Edit Index panel (list of edits and markers, double-click to jump).
- D2: source timecode on clips (a Timeline View Options entry).
- Transform controls: scale / rotate handles, anchor point, crop handles in the viewer.
- First play at once: a few frames dropped while the first hardware decoder warms up.
- T7 (round 1): speed-change nudges (no Resolve default keys known).
- The two-up trim view shows source frames (no program-rendered two-up).
