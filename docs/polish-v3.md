# Polish v3: basic editing on the Edit page

Roadmap v3 (docs/roadmap.md): make everyday editing feel finished. This page is the audit of
the Edit page against a normal Resolve editing session (import, browse, mark, insert /
overwrite, arrange, trim, blade, transitions, titles, audio levels, keyframes, render), and
the status of each item on the `editpolish` branch. Behaviour is decided from general
knowledge of Resolve (clean room); decisions are recorded in docs/behavior-spec.md.

Priorities: **P1** = noticed in every session, or a deferred spec item; **P2** = noticed
often; **P3** = nice to have / rare.
Status: **Done** (on this branch, with tests), **OK** (already fine, verified), **Open**
(not done in this round), **Partial**.

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
| V1 | Audio while shuttling (J / L at 2x-16x), in reverse and in slow play (K+J / K+L), pitched like a tape (spec 1.16, deferred). | P1 | Open |
| V2 | Audio scrubbing (Shift+S, on by default in Resolve): dragging the playhead or stepping frames plays a short snippet (spec 23.7, disabled). | P1 | Open |
| V3 | Typed timecode entry: with the timeline or the timeline viewer focused, typing digits enters a timecode (right-aligned like a calculator, `.` = `00`), Enter moves the playhead there; a leading `+` / `-` makes it an offset; with clips selected an offset moves the clips. | P1 | Open |
| V4 | Viewer edit overlay: dragging a clip (media pool, source viewer) onto the timeline viewer shows the edit types (insert, overwrite, replace, place on top, append at end, ripple overwrite, fit to fill); dropping on one performs it (spec 4.14, deferred). | P1 | Open |
| V5 | Two-up trim view: while trimming / rolling / slipping / sliding the timeline viewer shows the frames on both sides of the edit, with their timecodes (spec 8.15, deferred). | P1 | Open |
| V6 | J/K/L, frame stepping, K+J / K+L, ruler and scrub bar scrubbing. | - | OK (spec 1.x, 2.1; pe, tw) |
| V7 | Source viewer with in / out, drag to the timeline, double-click in the media pool opens the source viewer. | - | OK (spec 3.1, 3.9, 5.1, 24.5) |
| V8 | Match frame (F), replace edit (F11). | - | OK (spec 23.1, 4.7) |
| V9 | Replace edit lines up the source viewer's playhead with the timeline playhead when the source has no in mark (spec 4.7 Decision). | P2 | Open |

### Timeline

| # | Item | Pri | Status |
|---|---|---|---|
| T1 | Drag / trim info: while moving, trimming, rolling, slipping or sliding, a label next to the pointer shows the offset (`+00:00:01:05`) and the new timecode / duration. | P1 | Open |
| T2 | Alt+Up / Alt+Down move the selected clips one track up / down (linked clips of the other kind stay). | P2 | Open |
| T3 | Clip context menu: Change Clip Duration (Ctrl+D) is missing (Change Clip Speed is there). | P2 | Open |
| T4 | Track header context menu: Delete Empty Tracks (Resolve's "Delete Empty Tracks"). | P2 | Open |
| T5 | Track height presets in the header menu (small / medium / large for the track). | P3 | Open |
| T6 | Up / Down edit points, auto-select tracks, V / Shift+V / U, trim tool slip / slide, linked selection, snapping, gap selection and Delete Gaps, add / delete / rename tracks. | - | OK (spec 2, 6, 7, 8, 10, 11, 12) |
| T7 | Speed-change nudges (spec 29.16): Resolve has no default keys for them; commands without keys. | P3 | Open |
| T8 | Move Track Up / Down in the header menu. | P3 | Open |

### Inspector, media pool, responsiveness

| # | Item | Pri | Status |
|---|---|---|---|
| I1 | Reset buttons per parameter, scrubbable number fields (drag), typed entry (double-click), keyboard keys. | - | OK (ParamRows, ValueField; in) |
| I2 | Edits applying to every selected clip of the same kind (Resolve multi-clip inspector, spec 24.1 Decision). | P3 | Open |
| M1 | Media pool drag and drop to the timeline and to the viewers; Files from a file manager. | - | OK (spec 5, test_wayland) |
| R1 | Thumbnails and waveforms appear asynchronously (MediaCache), paint never blocks. | - | OK (TimelineMedia) |
| R2 | First play after import: decoders open on demand; measure the first-play delay on the owner's machine (test_playbackperf covers steady state only). | P3 | Open |
