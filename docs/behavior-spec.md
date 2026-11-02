# Edit page behaviour spec

What a user coming from DaVinci Resolve's Edit page expects Kinoklip's Edit page to do, written
in our own words from general knowledge of how NLEs behave (plan.md 5.10). Nothing here is copied
from Resolve documentation. Every item has the expected behaviour and Kinoklip's status, found
by testing it (unit / gesture tests, or the running app).

Status legend:
- **OK**: already behaved like this; verified.
- **Fixed**: differed or was missing; implemented on the `parity` branch.
- **Decision**: the convention is ambiguous or we deliberately differ; the reason is given and
  the behaviour is switchable where it is convention-dependent.
- **Deferred**: not done yet, with the reason (usually: needs a new subsystem).

Tests: `tp` = `tests/timeline/test_parity.cpp` (test names start with the item number),
`tc` = `test_timelinecontroller`, `tw` = `test_timelinewidget`, `pe` = `test_playbackengine`,
`vp` = `test_viewerpanel`, `eo` = `test_editops` / `test_editops_timeline`, `in` =
`test_inspector`.

Shortcuts are data (`resources/keymap_default.json`, remappable in Keyboard Customization);
convention switches live in `PlaybackBehaviour` (`src/playback/PlaybackEngine.h`, QSettings
`playback/*`) and `EditBehaviour` (`src/core/EditBehaviour.h`, QSettings `edit/*`).

---

## 1. Transport and J/K/L

| # | Expected behaviour | Status |
|---|---|---|
| 1.1 | Space toggles play/pause of the active viewer (the one used last: source or timeline). | OK (pe stateMachineAndShuttle, vp transportFollowsActiveViewer) |
| 1.2 | L plays forward at 1x; each further L doubles the speed (2x, 4x, 8x, capped). | OK (pe stateMachineAndShuttle) |
| 1.3 | J plays backwards the same way; pressing the opposite key while shuttling restarts that direction at 1x. | OK (pe) |
| 1.4 | K pauses. | OK (pe) |
| 1.5 | K held + a tap on L or J steps one frame forward or back. | OK (pe kHeldStepsAndSlowPlays) |
| 1.6 | K held + L or J held plays slowly forward / backward until a key is released. | OK (pe kHeldStepsAndSlowPlays; slow play after 300 ms at 0.5x) |
| 1.7 | Play pressed on the last frame starts over from the beginning instead of doing nothing. | OK (pe stopsAtEndAndRestarts; `PlaybackBehaviour::restartAtEnd`) |
| 1.8 | Playback stops on the last frame of the timeline when loop is off. | OK (pe stopsAtEndAndRestarts) |
| 1.9 | Shift+Space plays in reverse at 1x (again: pause). | Fixed (`transport.play_reverse`; tp spec1_keymapMatchesSpec) |
| 1.10 | Loop toggle: repeats in..out if both marks exist, else the whole timeline, seamlessly. | OK (pe loopsOverInOut; Ctrl+/) |
| 1.11 | Play in to out plays the marked range once. | OK (pe playInToOutStopsAtOut; Alt+/) |
| 1.12 | "Play around current frame" (/) plays a pre-roll before to a post-roll after the playhead and returns to it. | Fixed (key was Alt+Space, now /; pre/post-roll in `PlaybackBehaviour`) |
| 1.13 | Left / Right step one frame and pause; holding repeats. | OK (pe stepAndSeconds) |
| 1.14 | Shift+Left / Shift+Right move one second. | OK (pe stepAndSeconds) |
| 1.15 | Transport keys act on the source viewer when it has focus, on the timeline otherwise. | OK (vp transportFollowsActiveViewer) |
| 1.16 | Audio is heard while shuttling, in reverse and in slow play (pitched). | Deferred: the mixer has no rate parameter yet (TODO(playback) in src/playback/readme.md). |
| 1.17 | J/K/L ignore keyboard auto-repeat (holding L does not ramp to 8x). | OK (actions have auto-repeat off) |

## 2. Playhead and navigation

| # | Expected behaviour | Status |
|---|---|---|
| 2.1 | Clicking / dragging in the ruler moves the playhead; with snapping on it snaps to edit points, marks and markers. | OK (tw scrubRulerMovesPlayhead) |
| 2.2 | Up / Down jump to the previous / next edit point (clip start or end). | OK (tc navigation) |
| 2.3 | Only tracks whose auto track selector is on (and that are not locked) count for Up / Down. | Fixed (tp spec2_3_upDownOnlyAutoSelectTracks) |
| 2.4 | Home goes to the first frame, End to the frame after the last clip. | OK (tp spec2_4_homeEnd) |
| 2.5 | The playhead never goes before frame 0. | OK (tc navigation) |
| 2.6 | Clicking a clip with the selection tool selects it without moving the playhead. | OK (tw clickAndRubberBandSelection) |
| 2.7 | Shift+I / Shift+O go to the in / out mark (out: the last marked frame). | OK (tc marksAndMarkers) |
| 2.8 | Keyboard navigation reveals the playhead if it is outside the view. | OK (tw followPlayheadPageScrolls) |
| 2.9 | Shift+Up / Shift+Down jump to the previous / next marker. | Fixed (tp spec2_9_markerNavigation) |

## 3. Marking

| # | Expected behaviour | Status |
|---|---|---|
| 3.1 | I / O mark the viewer that has focus: the source clip in the source viewer, the timeline otherwise. | OK (vp sourceViewerLoadsClipAndMarks) |
| 3.2 | The out mark includes the frame under the playhead. | OK (tp spec3_2_3_outIncludesFrameAndInAfterOutClears) |
| 3.3 | Setting an in after the out (or an out before the in) clears the other mark. | OK (tp spec3_2_3) |
| 3.4 | Alt+I, Alt+O, Alt+X clear in, out, both. | OK (tc marksAndMarkers) |
| 3.5 | X marks in/out around the clip under the playhead (lowest auto-select video track first; a selected clip wins). | Fixed: used every track, now only auto-select tracks (tp spec3_5_markClipUsesAutoSelectTracks) |
| 3.6 | Shift+A marks in/out around the selected clips. | Fixed (tp spec3_6_markSelection) |
| 3.7 | The marked range is shown in the ruler and in the viewer's scrub bar. | OK (painter, ScrubBar; vp) |
| 3.8 | Timeline mark changes are undoable steps. | Decision: kept undoable (marks are saved with the timeline, so undo restoring them is the safer choice). |
| 3.9 | Source marks are remembered per clip. | OK (vp sourceViewerLoadsClipAndMarks) |

## 4. Source edits (insert, overwrite, ...)

| # | Expected behaviour | Status |
|---|---|---|
| 4.1 | F9 inserts the source range on the destination tracks, rippling every unlocked auto-select track by its length. | Fixed: rippled tracks whose auto-select was off too (tp spec4_1_insertRipplesOnlyAutoSelectTracks) |
| 4.2 | Three-point editing: with a timeline in mark the edit lands at the in mark, not at the playhead. | Fixed (tp spec4_2_editAtTimelineIn; `EditBehaviour::threePointEditing`) |
| 4.3 | With timeline in and out marks the timeline range sets the length (the source out is ignored; a shorter source makes a shorter clip). | Fixed (tp spec4_3_timelineInOutSetsDuration) |
| 4.4 | With only a timeline out mark the clip is backtimed: it ends at the out mark. | Fixed (tp spec4_4_timelineOutOnlyBacktimes) |
| 4.5 | F10 overwrites what is under the new clip. | OK (tc insertSourceOnTargets) |
| 4.6 | F12 place on top: the first free track above the destination, adding a track if needed. | OK (tc insertSourceOnTargets, eo placeOnTop*) |
| 4.7 | F11 replace: the clip under the playhead (destination video track first) is replaced by the source from its in point, keeping position and length. | Fixed (new; tp spec4_7_replaceKeepsPlaceAndLength). Decision: the source is aligned on its in mark; Resolve-style alignment of the source playhead with the timeline playhead is not done. |
| 4.8 | Shift+F11 fit to fill: source in..out is retimed (speed change) to fill timeline in..out. Needs both ranges. | Fixed (new; tp spec4_8_fitToFillRetimes) |
| 4.9 | Shift+F12 append at end: overwrite after the last clip of the timeline. | Fixed (new; tp spec4_9_appendAtEnd) |
| 4.10 | After a source edit the playhead sits after the new clip and the new clips are selected. | OK (tc insertSourceOnTargets, tp spec4_1) |
| 4.11 | Shift+F10 ripple overwrite (replace the range and ripple the length difference). | Deferred: rare; needs a new EditOps operation. |
| 4.12 | Video and audio of one source are placed together and linked. | OK (tc insertSourceOnTargets) |
| 4.13 | Stills get the standard still duration. | OK (tc stillsAndGenerators; `EditBehaviour::stillSeconds`) |
| 4.14 | Dragging a clip onto the timeline viewer shows an edit-type overlay (insert, overwrite, replace, ...). | Deferred: needs a viewer overlay widget. |

## 5. Drag and drop to the timeline

| # | Expected behaviour | Status |
|---|---|---|
| 5.1 | Dragging media or a marked source range onto a track overwrites there, linked V+A, with a ghost preview. | OK (tw dropMediaOverwritesAndCtrlInserts) |
| 5.2 | A modifier during the drop inserts instead of overwriting. | Decision: Ctrl inserts (no single convention is universal). |
| 5.3 | Effects dropped on a clip are applied to it; transitions dropped near an edge go to that edge. | OK (tw dropEffectsTransitionsAndTitles) |
| 5.4 | Titles / generators dropped on a video track create a clip of the standard generator length. | OK (tw dropEffectsTransitionsAndTitles) |

## 6. Destination controls and auto track selectors

| # | Expected behaviour | Status |
|---|---|---|
| 6.1 | Each track header has a destination patch (V1, A1, ...); clicking it targets that track for source edits. | OK (tc insertSourceOnTargets) |
| 6.2 | Untargeting video or audio leaves that part out of source edits. | OK (tc insertSourceOnTargets) |
| 6.3 | Each track has an auto track selector, on by default, saved with the project. | Fixed (new `TrackData::autoSelect`; tp spec6_3_autoSelectDefaultOnAndSaved) |
| 6.4 | Commands without an explicit target use the auto-select tracks: Up/Down (2.3), split at playhead without selection (9.2), mark clip (3.5), default transition (13.2), trim to playhead (8.10), and the all-track ripple of insert (4.1) and gap removal (10.5). | Fixed |
| 6.5 | The selector is a header button; clicking toggles it (one undo step). | Fixed (tp spec6_5_headerButtonTogglesAutoSelect) |
| 6.6 | Alt-click on a selector makes it the only one on of its kind. | Fixed (new; tp spec6_6_altClickSoloesAutoSelect) |
| 6.7 | Locked tracks are never changed by any edit. | OK (eo, `checkInvariants`; tw headerButtonsToggleTrackState) |

## 7. Selection

| # | Expected behaviour | Status |
|---|---|---|
| 7.1 | Clicking a clip selects it (and its linked clips when linked selection is on). | OK (tw clickAndRubberBandSelection) |
| 7.2 | Ctrl- (or Shift-) click adds or removes a clip. | OK (tw clickAndRubberBandSelection). Decision: Shift does the same as Ctrl (no range-select semantics). |
| 7.3 | Alt-click selects only the clicked part of a linked pair. | Fixed (tp spec7_3_altClickSelectsOneSide). Decision: an Alt-drag therefore duplicates just the clicked part. |
| 7.4 | Linked selection toggles with Ctrl+Shift+L (and the toolbar button). | OK (tw linkedClipsMoveTogether) |
| 7.5 | Dragging in an empty area draws a rubber band that selects every touched clip; with Ctrl/Shift it adds. | OK (tw clickAndRubberBandSelection) |
| 7.6 | Clicking a clip edge selects that edit point (highlighted); at a cut the centre selects both sides (roll). | Fixed (new; tp spec7_6_clickEdgeSelectsEditPoint) |
| 7.7 | Ctrl+A selects all clips, Ctrl+Shift+A deselects. | OK (tc selectAllAndDeselect) |
| 7.8 | Clicking empty track area deselects. | OK (tw clickAndRubberBandSelection) |
| 7.9 | Clip, edit point and gap selections are exclusive: selecting one clears the others. | Fixed (tp spec7_6, spec10_5) |
| 7.10 | Pressing on a clip inside a multi-selection keeps the group for dragging; a click without drag reduces the selection to that clip. | OK (tw) |

## 8. Trimming

| # | Expected behaviour | Status |
|---|---|---|
| 8.1 | Selection tool, dragging a clip edge: trims without ripple (leaves a gap; cannot overlap a neighbour). | OK (tw trimWithGapInSelectTool) |
| 8.2 | Selection tool, dragging the exact centre of a cut between two touching clips: roll. | Fixed (tp spec8_2_selectToolRollsAtCutCentre; `EditBehaviour::selectToolRollsAtCut`) |
| 8.3 | Trim tool, dragging an edge (either side of a cut): ripple trim of that side, later clips follow. | Fixed: the trim tool used to roll at every cut (tp spec8_3_trimToolRipplesEitherSideOfCut) |
| 8.4 | Trims are limited by the media handles and a minimum length of one frame. | OK (eo trim tests, rollClampedByMediaAndDuration) |
| 8.5 | Trim tool, centre of a cut: roll. | OK (tw rollAtEditPointInTrimTool) |
| 8.6 | Trim tool on a clip body: the upper name band slides the clip between its neighbours, the rest slips the content. | Fixed: was slip only, slide on Alt (Alt still swaps; tp spec8_6_trimToolNameBandSlides; `EditBehaviour::trimToolSlideInNameBar`) |
| 8.7 | Slip and slide are limited by media and neighbour lengths. | OK (eo slide*, tw slipInTrimTool) |
| 8.8 | , and . nudge the selected clips by one frame, or trim the selected edit point (roll if both sides; ripple in the trim tool). | Fixed: edit points could not be nudged (tp spec8_8_nudgeTrimsSelectedEdit) |
| 8.9 | Shift+, and Shift+. nudge / trim by several frames (5). | Fixed (new; tp spec8_8; `EditBehaviour::multiFrameNudge`) |
| 8.10 | Shift+[ / Shift+] trim the start / end of the clips under the playhead to the playhead (selected clips, else auto-select tracks), rippling. | Fixed (new; tp spec8_10_trimStartEndToPlayhead; `EditBehaviour::trimToPlayheadRipples`) |
| 8.11 | E extends the selected edit point to the playhead. | Fixed (new; tp spec8_11_extendEdit) |
| 8.12 | With linked selection on, linked clips whose edge sits on the same frame trim / roll along. | OK for drags (tw), Fixed for keyboard trims (tp spec8_10) |
| 8.13 | Esc cancels a drag without leaving an undo step. | OK (tw escapeCancelsDrag) |
| 8.14 | Trims snap to edit points / playhead when snapping is on. | OK (tw moveSnapsToEdges uses the same snap path) |
| 8.15 | While trimming, the viewer shows a two-up of the frames on both sides of the edit. | Deferred: needs a second render request path in the viewer. |
| 8.16 | U cycles the selected edit point between the outgoing side, the roll and the incoming side. | Fixed (new `trim.cycle_edit`; tp spec8_16_cycleEditPoint) |
| 8.17 | Dynamic (JKL) trimming. | Deferred: needs playback-driven trimming. |
| 8.18 | A ripple trim shifts the clips of the trimmed track (and of the clips trimmed along), not other tracks. | Decision: matches the common behaviour without sync locks; documented in EditOps. |

## 9. Blade

| # | Expected behaviour | Status |
|---|---|---|
| 9.1 | B selects the blade tool; clicking a clip cuts it (and its linked clips) at the frame under the pointer, snapping to the playhead when snapping is on; a hover line shows where. | OK (tw bladeSplitsLinkedClips) |
| 9.2 | Ctrl+B or Ctrl+\ splits at the playhead: the selected clips, or every auto-select track when nothing is selected. | Fixed: Ctrl+\ was missing, and it cut every track (tp spec9_2_splitAtPlayheadUsesAutoSelect) |
| 9.3 | Both parts keep effects, transforms and keyframes (keyframes follow the content). | OK (eo split tests) |
| 9.4 | Cutting exactly at a clip edge does nothing (no empty undo step). | OK (tc splitAtPlayheadAllOrSelected) |
| 9.5 | After cutting, the tool stays the blade until A or T is pressed. | OK |

## 10. Delete, ripple delete, gaps

| # | Expected behaviour | Status |
|---|---|---|
| 10.1 | Backspace removes the selection and leaves a gap. | OK (tc deleteAndRippleDeleteWithLinks) |
| 10.2 | Delete (and Shift+Backspace) ripple-deletes: the gap closes. | OK (tc deleteAndRippleDeleteWithLinks) |
| 10.3 | Linked parts are removed together when linked selection is on. | OK (tc) |
| 10.4 | Ripple delete shifts the tracks of the deleted clips only. | Decision: same reasoning as 8.18. |
| 10.5 | Clicking an empty range between clips selects that gap (highlighted); Backspace or Delete removes it; with nothing selected ripple delete does nothing. | Fixed: gaps could not be selected, and ripple delete without selection closed the gap under the playhead (still available: `EditBehaviour::rippleDeleteGapAtPlayhead`; tp spec10_5_gapSelectionAndDelete) |
| 10.6 | Removing a gap also shifts the other auto-select tracks by the same amount when they have room, keeping sync. | OK (eo closeGapAllTracksKeepsSync) |

## 11. Snapping

| # | Expected behaviour | Status |
|---|---|---|
| 11.1 | N toggles snapping (toolbar button reflects it). | OK (tc toolsAndTracks) |
| 11.2 | Snap targets: clip starts/ends, playhead, in/out marks, markers, timeline start. | OK (eo snapCandidates) |
| 11.3 | The snap distance is in pixels (8 px), so it feels the same at every zoom. | OK |
| 11.4 | A snap line shows while snapped. | OK (painter) |
| 11.5 | Pressing N during a drag turns snapping off / on for the rest of the gesture. | OK (the drag reads the live state; shortcut works during a mouse grab) |
| 11.6 | A moved clip snaps with whichever of its edges is closer to a target. | OK (tw moveSnapsToEdges) |
| 11.7 | Dragged clips never snap to themselves. | OK (exclusion list) |

## 12. Track headers

| # | Expected behaviour | Status |
|---|---|---|
| 12.1 | Lock button: a locked track cannot be edited. | OK (tw headerButtonsToggleTrackState) |
| 12.2 | Video: enable/visibility button hides the track from the output. | OK (tw headerButtonsToggleTrackState) |
| 12.3 | Audio: mute and solo buttons. | OK (tw headerButtonsToggleTrackState) |
| 12.4 | Dragging the bottom edge of a header changes the track height (one undo step). | OK (tw resizeTrackByDraggingHeaderEdge) |
| 12.5 | Auto track selector button (see 6.3-6.5). | Fixed |
| 12.6 | Track name editable; context menu adds / deletes tracks. | OK (tc toolsAndTracks) |
| 12.7 | Header button changes are undoable. | OK (tp spec6_5) |

## 13. Transitions

| # | Expected behaviour | Status |
|---|---|---|
| 13.1 | Standard transition: cross dissolve, 1 second. | OK (`EditBehaviour::transitionSeconds`, tp spec13_2) |
| 13.2 | Ctrl+T adds it at the selected edit point; with clips selected at both ends of each; otherwise at the edit nearest to the playhead on the auto-select video tracks. Existing transitions are kept. | Fixed: used only the nearest edge of the selection, ignored edit points (tp spec13_2_defaultTransitionPlacement) |
| 13.3 | Linked audio gets a matching cross fade. | OK (the mixer fades linked audio with the video transition) |
| 13.4 | At a cut the transition is centred on the cut. | OK (render TimelineEval) |
| 13.5 | Alignment can also be "ending at" or "starting at" the cut. | Deferred: needs an alignment field in TransitionData and render support. |
| 13.6 | At an edge without a neighbour it fades from / to black (transparent). | OK (render) |
| 13.7 | Without enough handles the user is told and offered a shorter transition / trimmed clips. | Deferred: today the transition is added and the missing frames repeat the edge frame. |
| 13.8 | Transition length editable in the inspector. | OK (in transitionTab) |
| 13.9 | Dragging a transition's edges in the timeline changes its length. | Deferred: needs a new drag mode. |

## 14. Titles, generators, stills

| # | Expected behaviour | Status |
|---|---|---|
| 14.1 | New titles and generators are 5 s long. | OK (tc stillsAndGenerators, tp spec14_1; `EditBehaviour::generatorSeconds`) |
| 14.2 | Stills are 5 s long. | OK (tc stillsAndGenerators; `EditBehaviour::stillSeconds`) |
| 14.3 | Title text and style are edited in the inspector, one undo step per typing burst. | OK (in titleClipTextIsOneUndoStepPerBurst) |
| 14.4 | Titles and generators can be trimmed to any length (no media limit). | OK (eo, sourceDurationUs = 0) |

## 15. Keyframes (inspector)

| # | Expected behaviour | Status |
|---|---|---|
| 15.1 | A diamond per animatable parameter: dim (no keys), outlined (keys, none here), filled (key at the playhead). | OK (in keyframeButtonTogglesAtLocalFrame) |
| 15.2 | Clicking the diamond adds / removes a key at the playhead. | OK (in keyframeButtonTogglesAtLocalFrame) |
| 15.3 | Changing an animated value adds or updates a key at the playhead. | OK (in scrubOnAnimatedParamSetsKey) |
| 15.4 | Arrows (and [ / ]) jump to the previous / next key. | OK (in, `clip.prev_keyframe/next_keyframe`) |
| 15.5 | The diamond is disabled when the playhead is outside the clip. | OK (in) |
| 15.6 | Keys interpolate linearly by default; the interpolation (linear, ease in, ease out, ease in and out, hold) can be changed per key. | Fixed: no UI existed; right-click on a filled diamond (in keyframeInterpolationMenu) |
| 15.7 | Keys move with the clip and follow the content on head trims and splits. | OK (eo) |
| 15.8 | Keyframes can be shown and dragged on the timeline / in a curve editor. | Deferred: needs a keyframe lane. |

## 16. Clipboard

| # | Expected behaviour | Status |
|---|---|---|
| 16.1 | Ctrl+C / Ctrl+X / Ctrl+V copy, cut (leaves a gap), paste. | OK (tc copyCutPaste) |
| 16.2 | Paste overwrites at the playhead on the tracks the clips came from; the playhead moves after the pasted range; pasted clips are selected with fresh ids and links kept. | OK (tc copyCutPaste, eo copyPasteKeepsLayoutWithFreshIds, tp spec16_3) |
| 16.3 | Ctrl+Shift+V paste insert ripples the timeline to make room. | Fixed (new; tp spec16_3_pasteInsertRipples) |
| 16.4 | Paste attributes (Alt+V). | Deferred: needs an attribute picker dialog. |

## 17. Undo

| # | Expected behaviour | Status |
|---|---|---|
| 17.1 | Each gesture or command is one undo step. | OK (tw dragMoveIsOneUndoStep, tc, tp) |
| 17.2 | Cancelled gestures and no-op commands leave no undo step. | OK (tw escapeCancelsDrag, tc) |
| 17.3 | Selection, playhead, zoom and scroll are not undo steps. | OK |
| 17.4 | Ctrl+Z / Ctrl+Shift+Z; undo is ignored in the middle of a drag. | OK (test_project) |

## 18. Markers

| # | Expected behaviour | Status |
|---|---|---|
| 18.1 | M adds a timeline marker at the playhead (never two on one frame). | OK (tc marksAndMarkers) |
| 18.2 | Markers are drawn in the ruler and are snap targets. | OK |
| 18.3 | Navigation with Shift+Up / Shift+Down (2.9). | Fixed |
| 18.4 | With a clip selected, M adds a clip marker. | Deferred: needs clip markers in the data model. |
| 18.5 | Pressing M on an existing marker opens its name / colour / note editor. | Deferred: needs a marker dialog. |

## 19. Zoom and scrolling

| # | Expected behaviour | Status |
|---|---|---|
| 19.1 | Ctrl+= / Ctrl+- zoom around the playhead when it is visible. | OK (tw zoomControls) |
| 19.2 | Shift+Z fits the whole timeline. | OK (tw zoomControls) |
| 19.3 | Wheel scrolls tracks (or time when they fit), Shift+wheel scrolls time, Ctrl+wheel zooms at the pointer. | Decision: wheel conventions vary by editor and preference; kept, to become a profile switch. |
| 19.4 | Middle-drag pans. | OK |
| 19.5 | Zoom limits from about 2.5 h per 1000 px to 48 px per frame. | OK |

## 20. Timeline follow during playback

| # | Expected behaviour | Status |
|---|---|---|
| 20.1 | During playback the view page-scrolls when the playhead leaves it. | OK (tw followPlayheadPageScrolls) |
| 20.2 | The view does not jump while the user scrubs. | OK |

## 21. Moving clips

| # | Expected behaviour | Status |
|---|---|---|
| 21.1 | Dragging a clip moves it; whatever it lands on is overwritten (exact while dragging back and forth). | OK (tw dragMove*, dragBackAndForthDoesNotChewNeighbours) |
| 21.2 | Dragging vertically changes track within the same type; linked clips of the other type keep their track. | OK (tw linkedClipsMoveTogether) |
| 21.3 | Alt-drag duplicates. | OK (tw altDragDuplicates) |
| 21.4 | Clips cannot be moved before frame 0 or onto / off locked tracks. | OK (eo moveClips) |
| 21.5 | D toggles the selected clips' enabled state. | OK (tc nudgeAndToggleEnabled) |
| 21.6 | Ctrl+Shift drag performs a swap / shuffle insert move. | Deferred: needs a new EditOps operation. |

## 22. End-to-end flow (running app)

`tests/app/test_e2eflow.cpp` drives the real main window offscreen (program engine without a
sound device) through the widgets and actions a user touches:

1. import `h264_25fps.mp4` (10 s, 1280x720, AAC) into the media pool;
2. drop it from the pool onto V1 (linked V+A, 250 frames);
3. trim the tail by 50 frames with the selection tool (audio follows);
4. Ctrl+B at frame 100 (both tracks cut), drag the right half 25 frames later;
5. click the gap, Backspace: the gap closes on V1 and A1;
6. drop a Text title on V2 (125 frames), Ctrl+T near the cut (25-frame dissolve);
7. two opacity keys through the inspector diamond;
8. Space: the engine plays, the playhead advances, frames reach the timeline viewer;
9. Deliver page, YouTube 1080p preset, Add to Render Queue, Render All: the job finishes and
   the file probes as 1920x1080 with audio, 8.0 s long.

Result: passes. Integration bug found and fixed on the way: without a restored layout
(fresh start, `--screenshot` mode) the main window opened on the Media page and the Edit
page's widgets kept their unshown default sizes (`MainWindow` now selects Edit explicitly).

---

## Summary

- 160 numbered items (12.5 and 18.3 repeat 6.5 and 2.9 for readers of those sections).
- OK: 107 (already right; verified by existing or new tests).
- Fixed on the `parity` branch: 35.
- Decision: 5 pure decisions plus decision notes on 4.7, 7.2 and 7.3.
- Deferred: 13 — 1.16 audio while shuttling, 4.11 ripple overwrite, 4.14 viewer edit
  overlay, 8.15 two-up trim view, 8.17 dynamic trimming, 13.5 transition alignment, 13.7
  insufficient-handles prompt, 13.9 transition edge drag, 15.8 timeline keyframe lane, 16.4
  paste attributes, 18.4 clip markers, 18.5 marker editor, 21.6 swap / shuffle move. Each needs
  a new subsystem (UI surface, data field or render path) rather than a behaviour fix.
