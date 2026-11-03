# Polish v3: basic editing on the Edit page

Roadmap v3 (docs/roadmap.md): make everyday editing feel finished. This page is the audit of
the Edit page against a normal Resolve editing session (import, browse, mark, insert /
overwrite, arrange, trim, blade, transitions, titles, audio levels, keyframes, render), and
the status of each item on the `editpolish` branch. Behaviour is decided from general
knowledge of Resolve (clean room); decisions are recorded in docs/behavior-spec.md.

Priorities: **P1** = noticed in every session, or a deferred spec item; **P2** = noticed
often; **P3** = nice to have / rare.
Round 1 was the `editpolish` branch, round 2 the `polish2` branch (section "Round 2" below),
round 3 the `polish3` branch (section "Round 3"), round 4 the `polish4` branch (section
"Round 4").

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
| T7 | Speed-change nudges (spec 29.16): Resolve has no default keys for them; commands without keys. | P3 | Done in round 3 (spec 29.17; p3) |
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
| D2 | Source timecode on clips. | P3 | Done in round 3 (spec 12.14; p3), off by default like Resolve |
| M1 | Markers: M / Ctrl+M / Alt+M, colour and name editor, Shift+Up / Down, clip markers. | - | OK (spec 18) |
| M2 | Marker list in the Edit Index: the Edit Index is still a placeholder panel. | P2 | Done in round 3 (spec 18.8, 18.9; ei) |
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

## Round 3: the round 2 leftovers and a last pass over a basic session

Branch `polish3` (after round 2). First the leftovers of round 2, then a short audit of what
is still rough in a basic session (import, cut, trim, titles, markers, render). Tests: `ei` =
tests/panels/test_editindex.cpp, `tc3` = tests/playback/test_transformcontrols.cpp, `p3` =
tests/timeline/test_polish3.cpp, `tu` = tests/playback/test_trimtwoup.cpp (also the two-up
benchmark), `fp` = tests/app/test_firstplay.cpp, `mp` = test_mediaperf filmstripLatency.

### Round 2 leftovers

| # | Item | Pri | Status |
|---|---|---|---|
| L1 | Edit Index panel: the events of the timeline (#, reel, name, track, source in / out, record in / out, duration, notes) and a Markers mode (colour, name, timecode, duration, notes, location); click to jump and select, search, colour filter, marker name / notes edited in place (one undo step), live, scheme colours. | P2 | Done (spec 18.8, 18.9; ei) |
| L2 | On-screen Transform controls complete: corner / edge scale handles, rotation knob, anchor point; a Crop mode with edge and corner handles. One named undo step per drag, keys at the playhead when animated. | P2 | Done (spec 24.11, 24.12; tc3) |
| L3 | Source timecode on clips (Timeline View Options), off by default. | P3 | Done (spec 12.14; p3) |
| L4 | Two-up trim view with the program look (effects, transforms) at reduced size. | P3 | Partial: implemented, opt-in (spec 8.19 Decision; tu): it lags fast drags, so source frames stay the default (numbers below) |
| L5 | Increase / Decrease Clip Speed by 10% (commands without keys). | P3 | Done (spec 29.17; p3) |
| L6 | First play right after an import: the few dropped frames. | P2 | Done (spec 1.19; fp, mp; numbers below) |

Decisions (details in the spec):
- Scale handles (24.11): corners keep the aspect while Zoom X / Y are linked (the Inspector's
  default) and edges then scale both axes; **Shift toggles** the link for the drag, as
  Resolve's linked zoom behaves (a free scale of a linked zoom unlinks it). Shift snaps a
  rotation to 15 degree steps and keeps a move on one axis. The anchor cross moves the anchor
  point while the picture stays still (Position compensates). Scaling and rotating pivot on
  the anchor point.
- Edit Index (18.8, 18.9): one row per clip (linked video and audio are separate events, as
  on the tracks); Notes of an event are read-only (transitions, speed, disabled, offline);
  markers are single frames, so Duration shows one frame.
- Speed nudges (29.17): percentage points (100 % -> 110 % -> 120 %), at least 10 %; reverse,
  freeze and ramped clips are left alone.

### Two-up with the program look (L4)

`tests/playback/test_trimtwoup.cpp rollDragCost`: a roll dragged one frame per 16 ms (40
steps) over a 1080p H.264 pair, the left clip blurred, the right one zoomed; "up to date" =
both sides show the wanted frame at the next step. Measured on the Xvfb display (llvmpipe
OpenGL; offscreen there is no GPU compositor and the view stays in Source mode).

| Mode | GUI time per step | Sides up to date after a step | Catch up after the drag |
|---|---|---|---|
| Source frames (default) | 0.07 ms | 68 % | 0 ms |
| Program look | 0.05 ms | 0 % | 160 - 200 ms |

Why not the default: the program renders go through the viewer's one render thread, next to
the viewer's own still frames (three renders per mouse move), so on this machine they never
kept up with the drag. On a real GPU each render is far cheaper, so it is a preference
(Editing > Trimming, "Trim view shows effects and transforms (slower)") rather than removed.

### First play after import (L6)

Same test as round 2 (`tests/app/test_firstplay.cpp`, real main window, offscreen with the CPU
compositor, CUDA decoding on the owner's laptop), two runs each, Space pressed right after
the drop. The perf clips are those of scripts/make_perf_media.sh.

| Clip | Before: frames dropped | After | Before: first frame after play | After |
|---|---|---|---|---|
| h264 720p25 | 5, 3 | 0, 0 | 121 - 173 ms | 74 - 78 ms |
| screen 1080p60 | 5, 6 | 0, 0 | 175 - 223 ms | 124 - 127 ms |
| hevc 2160p30 | 6, 6 | 1, 2 | 172 - 225 ms | 123 - 124 ms |

Space after the paused frame appeared stays at 51 ms and 0 dropped. Cause: the drops did not
come from the playback decoder itself but from the media cache: the new clip's first
filmstrip opened up to four hardware decoders (one per preview thread) at the moment the
playback opened its own, and with thumbnails off the first play dropped nothing. Preview
decoders now decode in software up to 1920x1200 and use the hardware decoder only for larger
pictures (`MediaCache::PreviewDecoding::BySize`). Filmstrip of 20 cold 64 px thumbnails
(`test_mediaperf filmstripLatency`), software / hardware: perf 1080p H.264 0.42 / 0.61 s,
screen 1080p60 0.59 - 0.63 / 0.95 - 2.4 s, hevc 2160p30 2.9 - 3.1 / 1.2 - 1.3 s, 720p25
0.4 - 0.8 / 0.3 - 0.4 s; so small files are as fast or faster in software and 4K HEVC keeps
the hardware decoder. Pre-opening the playback decoder (the other idea) was not needed: the
decoder under the playhead is already open by the time Space is pressed after the paused
frame, and at once the still render opens it anyway.

### Audit

| # | Item | Pri | Status |
|---|---|---|---|
| A1 | Edit Index, Markers: a row selects the marker's clip, and Delete then reached the window's Delete commands and ripple deleted the clip instead of the marker. | P1 | Done: Delete / Backspace delete the marker, one undo step (spec 18.9; ei deleteKeyDeletesTheMarker) |
| A2 | On-screen Transform / Crop controls hid themselves when the audio part of a linked clip was clicked last (the Inspector shows the pair as one item). | P1 | Done: they act on the video part (spec 24.11; tc3 linkedAudioPartShowsTheVideoControls) |
| A3 | Clip > Normalize Audio Levels (Resolve's clip menu; sets the clip gain so the peak reaches a target). | P2 | Done in round 4 (spec 37.35; test_audionormalize) |
| A4 | Edit Index export: Resolve exports the list as an EDL / marker list. | P3 | Done in round 4 (spec 18.12, 18.13; test_edlexport) |
| A5 | Edit > History (a list of the undo steps to jump back several at once). | P3 | Done in round 4 (spec 17.6; p4) |
| A6 | Edit Index columns are fixed (Resolve lets the user hide columns from the panel menu). | P3 | Done in round 4 (spec 18.10; p4) |
| A7 | Ranged markers (a marker with a duration, Alt+drag in the ruler). | P3 | Done in round 4 (spec 18.11; p4; project format 13) |
| A8 | Typed timecode, J/K/L, Space and the marker / speed commands with the Edit Index focused. | - | OK (window-scoped commands; the table only takes Delete in Markers mode and text keys while a cell is edited) |

### Shortcuts changed in round 3

| Command | Key | Confidence |
|---|---|---|
| On-Screen Crop Controls (`view.onscreen_crop`) | none (new command; the viewer's on-screen controls menu) | unsure |
| Increase Clip Speed by 10% (`clip.speed_increase`) | none (new command; Clip menu) | unsure |
| Decrease Clip Speed by 10% (`clip.speed_decrease`) | none (new command; Clip menu) | unsure |

### Left after round 3

- A3 Normalize Audio Levels, A4 Edit Index export (EDL), A5 Edit > History, A6 Edit Index
  column chooser, A7 ranged markers.
- The program-look two-up stays opt-in until it is measured on a real GPU (the benchmark
  skips without a GPU compositor; run it on a desktop session).
- 2160p HEVC first play at once still drops 1 - 2 frames (the 4K previews keep the hardware
  decoder).

## Round 4: the round 3 leftovers, a large-project stress pass and a last audit

Branch `polish4` (after 0.3.1-alpha.1). Tests: `p4` = tests/timeline/test_polish4.cpp,
`lp` = tests/timeline/test_largeproject.cpp (the stress benchmark), `us` =
tests/core/test_undosnapshots.cpp, test_edlexport, test_audionormalize, test_polish4shots
(the real main window; screenshots with KK_SCREENSHOT_DIR).

### Round 3 leftovers

| # | Item | Pri | Status |
|---|---|---|---|
| L1 (A3) | Clip > Normalize Audio Levels: Sample Peak Program / True Peak / ITU-R BS.1770-4, target level, Relative / Independent for several clips; measured off the GUI thread with the Audio page's `dsp::LoudnessMeter`, progress bar, Cancel; sets `audio/volumeDb` in one undo step. | P2 | Done (spec 37.35; test_audionormalize) |
| L2 (A7) | Ranged markers: Alt+drag a ruler marker or drag a ranged marker's end, Duration in the marker editor; bands on the ruler and bars on clips, Edit Index Duration, snap to both ends. | P3 | Done (spec 18.11; p4; format 13) |
| L3 (A4) | File > Export > Timeline as EDL (CMX3600 V, A, A2, dissolves, M2), Timeline Markers to EDL / CSV. | P3 | Done (spec 18.12, 18.13; test_edlexport parses the EDL back) |
| L4 (A5) | Edit > History > Open History Window (QUndoView, scheme colours, click to jump). | P3 | Done (spec 17.6; p4) |
| L5 (A6) | Edit Index column chooser on the header's context menu, stored per mode. | P3 | Done (spec 18.10; p4) |

Decisions (details in the spec):
- Ranged markers (18.11): Alt+drag on the flag sets the end (Resolve's gesture); the end of
  a ranged marker can also be dragged without Alt. Shift+Up / Down stop at the start only.
  Clip markers get a duration from the marker editor (Alt+drag on a clip body moves clips).
  Format 13 writes "duration" only when it is above one frame.
- EDL (18.12): one video track per EDL (CMX3600 has one), V1 by default; reels are the file
  name cut to 8 characters, "AX" for sources without a file; every timecode in the timeline
  rate; a ramp is written at its average speed with a comment.
- Normalize (37.35): Independent is the default; "Relative" applies one gain so the loudest
  clip reaches the target. The source audio is measured (before the clip volume), so the
  result does not depend on the current volume; keyframed volumes move by the same change.

### Large-project stress pass

`tests/timeline/test_largeproject.cpp` builds a synthetic project (`LargeProject.h`): one hour
at 25 fps, 8 video and 8 audio tracks, 1,492 clips (V1 / A1 a linked cut, sparser upper
tracks), 284 transitions, 614 keyframes, 254 markers, 40 media items. Measured on the owner's
laptop, Release, offscreen raster, the timeline 1920 px wide. Before = main at 0.3.1-alpha.1
(only the benchmark added), after = this branch.

| Measure | Before | After |
|---|---|---|
| Memory: project in RSS / per undo step (blade on every track) | +4.3 MB / +0.85 MB | +5.1 MB / +0.16 MB |
| Save (1.2 MB file) / load | 26.8 ms / 12.2 ms | 24 ms / 11 ms |
| Autosave: GUI thread blocked | 25.8 ms | 0.2 ms (write on a worker) |
| Paint while scrolling, fully zoomed out (mean / worst) | 7.5 / 9.0 ms | 6.6 / 8.0 ms |
| Paint while scrolling, 2 px/frame | 3.4 / 3.9 ms | 2.9 / 3.3 ms |
| Paint while scrolling, 10 px/frame | 2.6 / 3.0 ms | 2.2 / 2.6 ms |
| Paint while zooming (12 steps, worst) | 12.6 ms | 11.1 ms |
| Blade incl. views and one paint (Edit Index open) | 17.6 ms | 11.9 ms |
| Ripple delete incl. views / the edit alone | 18.3 / 0.4 ms | 12.2 / 0.7 ms |
| Undo / redo incl. views | 19.1 / 17.9 ms | 12.2 / 12.0 ms |
| Edit Index rebuild: events (1,492) / markers / whole panel | 5.8 / 5.8 / 6.3 ms | 1.2 / 0.06 / 1.7 ms |
| Animator: modelChanged after a ripple delete / tick | 0.19 / 0.001 ms | 0.20 / 0.001 ms |

The paint and save gains come from the faster `timecode::format()` (ruler labels, JSON is
unchanged). The edit alone got 0.3 ms slower: packing the undo step (comparing the clips
with the step below) is the price of the memory saving. The benchmark's ceilings: save
400 ms, load 600 ms, autosave block 50 ms, paint 60 ms, edit with views 100 ms, Edit Index
60 ms, animator 30 ms, 8 MB per undo step.

Findings and fixes:
- **Undo memory** was the real problem: the snapshot undo kept a full `TimelineData` copy
  per step (0.85 MB here, so 1,000 edits = 850 MB). Steps are now stored packed
  (`core/UndoSnapshots.h`): a clip-less shell plus shared immutable clips, packed against the
  neighbouring step; a step owns only what it changed. The packing compares clips by id
  and value (a fraction of a millisecond here); undo copies once, or reuses the live data.
- **Autosave** built the JSON and wrote the file on the GUI thread (21 - 26 ms here, more for
  longer projects). The timer now captures a `Project::SaveSnapshot` (shared immutable values)
  and builds / writes the file on a worker; a discard during the write removes the file.
- **Edit latency** was dominated by the Edit Index rebuild (both modes, two
  `QString::arg()` timecodes per row, three media lookups per row). Now only the shown mode
  is built, media facts are looked up once per item, notes are skipped when empty and
  `timecode::format()` writes into a stack buffer.
- Timeline painting, project load / save and the animator were already well inside a
  frame; the animator's guards (50,000 clips / 200 appearing) never trigger here. Nothing was
  changed there. The remaining paint cost is text shaping of clip names (audit A17).

### Audit

| # | Item | Pri | Status |
|---|---|---|---|
| A9 | Markers could not be moved: a drag on a ruler marker scrubbed the playhead. | P1 | Done: dragging the flag moves it, a click jumps to it (spec 18.14; p4) |
| A10 | Clip markers could only be edited with M on the playhead with the clip selected. | P2 | Done: double-click on the diamond (spec 18.15; p4) |
| A11 | Hovering a marker showed nothing (its name / notes only in the editor or the Edit Index). | P2 | Done: tool tip with name, timecode, duration, notes (spec 18.15) |
| A12 | Undo memory grew by a full timeline copy per step on long timelines. | P1 | Done (spec 17.7; us, lp) |
| A13 | Autosave blocked the GUI thread for the whole JSON build and write. | P1 | Done (background write; lp) |
| A14 | Every edit rebuilt both Edit Index modes, the largest part of the edit latency on long timelines. | P2 | Done (lp) |
| A15 | Marker Duration cannot be typed in the Edit Index table (name and notes can). | P3 | Open |
| A16 | EDL export writes one video track; no AAF / FCPXML interchange. | P3 | Open (Decision: CMX3600 has one video track) |
| A17 | Timeline paint shapes every visible clip name on every paint (6 - 7 ms fully zoomed out on 1,500 clips). | P3 | Open (inside a frame; cache shaped names later) |
| A18 | Timeline markers stay put on ripple edits. | - | OK (Resolve behaves the same) |

### Shortcuts changed in round 4

| Command | Key | Confidence |
|---|---|---|
| Open History Window (`edit.history_window`) | none (Edit > History) | likely |
| Export Timeline as EDL (`file.export_edl`) | none (File > Export) | likely |
| Export Timeline Markers to EDL (`file.export_markers_edl`) | none | likely |
| Export Timeline Markers to CSV (`file.export_markers_csv`) | none | unsure |
| Normalize Audio Levels (`clip.normalize_audio`) | none (Clip menu, clip context menu) | likely |

Gestures (not commands): Alt+drag a ruler marker (duration), drag a marker flag (move),
double-click a clip marker (editor).

### Left after round 4

- A15 Duration editing in the Edit Index, A16 multi-track interchange (AAF / FCPXML), A17
  cached shaped clip names.
- The program-look two-up and the 4K HEVC first-play drops from round 3.
