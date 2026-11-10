# src/ui/timeline — timeline editor

The Edit page's timeline: a custom-painted, interactive track view (no QGraphicsView) plus the
commands that edit the timeline. Depends on `core`, `ui/common` and `media` (MediaCache only,
for thumbnails and waveforms).

## Files

| File | What |
|---|---|
| `TimelinePanel.h` | `PanelFrame` with the tool bar, owns the controller and the widget; wires the zoom actions |
| `TimelineController.h` | commands (split, delete, ripple delete, nudge, marks, markers, copy/cut/paste, default transition, link, enable), editor state (tool, snapping, linked selection, track targeting), `insertSource()` for F9/F10/F12, clip factories; connects the actions this module owns |
| `VimTimelineHost.h` | the vim layer's host for a timeline: triggers registry actions, moves the playhead, selects the clip(s) at the playhead or a range, moves the target video track |
| `TimelineWidget.h` | `QAbstractScrollArea`; the view. Implementation split in three files: `TimelineWidget.cpp` (setup, paint, zoom, scroll, follow playhead, context menus), `TimelineInput.cpp` (mouse state machine), `TimelineDrop.cpp` (drag and drop) |
| `TimelineCommands.cpp` | controller part 2: delete / ripple delete / ripple cut of clips, gaps and in-out ranges, clipboard, select forward / nearest edit / nearest clip, join, swap, match frame, track targeting by number, clip duration / speed |
| `TimelineTransitions.cpp` | controller part 3: Ctrl+T with the insufficient-handles question, transition alignment and length |
| `TimelineMarkers.cpp` | controller part 4: timeline / clip markers and the marker editor hook, paste attributes, dynamic trim |
| `TimelineGestures.cpp` | widget gestures: transition edge drag, Ctrl+Shift shuffle drag, ruler marker double-click; markers: move by dragging the flag (18.14), Alt+drag / end drag for ranged markers (18.11), clip marker hit test, marker tool tips (18.15) |
| `TimelineDragInfo.cpp` / `DragInfo.h` | widget part: the offset label next to the pointer while moving / trimming (spec 21.9) and the `TrimPreview` for the two-up view (`trimPreviewChanged()`, spec 8.15) |
| `TimelineEntryEdits.cpp` | controller part: typed timecodes (`enterTimecode()`, spec 2.10) and the viewer edit overlay's edits (`editSources()`, spec 4.14) |
| `TimelineTrackCommands.cpp` | controller part: Alt+Up / Down clip moves (spec 21.10), Delete Empty Tracks (12.8), Move Track Up / Down (`moveTrack()`, 12.9), track height presets (`setTrackHeightPreset()`, 12.10) |

Timeline View Options (spec 12.11): `TimelinePanel` owns the button at the left of the tool
bar; the view draws video clips per `TimelineWidget::setClipThumbnails()` (`ClipThumbnails`:
filmstrip, head and tail, none) and audio waveforms per `setShowWaveforms()`. Clips of offline
media (`MediaPool::isOffline()`) are drawn red with "Media Offline" (12.13). Show Source
Timecode (12.14, off by default, `setShowSourceTimecode()`) draws the source in timecode after
the clip name; `TimelinePainter::sourceTimecodeText()` caches the text per clip
(`ui/common/SourceTimecode.h` gives the media's start timecode and rate).
| `TimelineDialogs.h` | the modal questions (handles, marker editor, paste attributes, name of a new timeline / compound clip, Timeline Settings) installed by the panel |
| `TimelineCompound.cpp` | controller part 5: New Timeline / Duplicate / Delete, tab cycling and closing, New Compound Clip, Open in Timeline, Decompose in Place |
| `TimelineTabs.h` | the row above the tracks: tabs of the open timelines (switch, drag to reorder, close, context menu) or, with tabs off (Preferences > Editing), a drop-down |
| `NestedPreview.h` | thumbnails / waveforms of compound clips and nested timelines, rendered in the background from the nested timeline and cached until it changes; the painter reaches it through the `kk-nested:<uid>` pseudo path |
| `TimelineEditUtil.h` | private: `runEdit()` (one undo step or none), `applyClipSpeed()`, `applyClipRetime()` (paste attributes) |
| `TimelineRetime.cpp` | controller part 6: retime targets, Change Clip Speed, Ctrl+R retime controls state, Shift+R freeze frame, speed bar commands |
| `RetimeBar.h` | pure geometry / hit-testing of the retime controls (speed bar fields, speed point handles, end handle) |
| `TimelineRetimeInput.cpp` | widget part: speed bar presses, handle drags (`DragMode::RetimePoint`), the speed menu and speed point menu, Retime Curve (curve lane with only the retime curve) |
| `ChangeSpeedDialog.h` | the R dialog: speed / fps / duration linked, reverse, freeze, ripple, pitch correction |
| `TimelineLayout.h` | pure geometry: frame <-> x, rows, header buttons, clip ranges, hit-testing (unit-tested) |
| `TimelinePainter.h` | draws ruler, headers, clips, transitions, overlays for one paint event |
| `TimelineAnimator.h` | display-only tweens (v2.2, spec 36): clips sliding after committed edits, appearing / ghost fade-out, blade and trim flashes, edit point pulse, snap fade, playhead glide |
| `TimelineMedia.h` | thumbnail / waveform provider types and the MediaCache-backed defaults |
| `KeyframeLanes.h` | per-clip keyframe / curve editor state, lane heights for the layout, lane widget placement, the two toggle buttons on animated clips, keyframe commands and actions |
| `KeyframeTrackWidget.h` | keyframe editor lane: a `DopeSheetView` (src/ui/keyframes) of one clip with the row labels in the header column |
| `CurveEditorWidget.h` | curve editor lane: a `CurveView` (src/ui/keyframes) of one clip with the parameter list and preset tool bar in the header column |

Tests: `tests/timeline/` (layout maths, controller commands, QTest gestures, demo project,
2,000-clip benchmark) and `tests/core/test_editops_timeline.cpp` (roll, slide, gaps,
place on top, clipboard).

## Public API for other modules

```cpp
TimelinePanel* panel = ...;                       // EditPage owns it
TimelineController* tc = panel->controller();
// Source viewer: F9/F10/F12 (edit.insert/overwrite/place_on_top) ask this callback for the
// current clip and range; or call insertSource() directly.
tc->setSourceProvider([viewer] { return std::optional<SourceSelection>({id, inUs, outUs}); });
tc->insertSource(mediaId, inUs, outUs, SourceEditMode::Insert);  // at the playhead, on the
                                                                 // targeted tracks; one undo step
TimelineWidget* w = panel->timelineWidget();
w->setFollowPlayhead(true);          // default: page-scroll when the playhead leaves the view
w->ensurePlayheadVisible();
w->setThumbnailProvider(fn);         // default: media::MediaCache::thumbnail()
w->setWaveformProvider(fn);          // default: media::MediaCache::waveform()
w->refreshMedia();                   // repaint (already connected to MediaCache's signals)
// Another page's timeline view on the same controller (the Cut page's lower timeline):
auto* lower = new TimelineWidget(ctx, tc);
lower->setViewOptions({.centredPlayhead = true, .rippleEdgeTrims = true,
    .audioHeightScale = 0.55, .keepsViewState = false});
// Page-specific meaning of F9 / F10 / ... (the Cut page's smart edits while it is shown):
tc->setSourceEditHandler([page](SourceEditMode m) { return page->isVisible() && ...; });
```

`TimelineViewOptions` (TimelineWidget.h) are per view: a centred playhead (the view scrolls
under the playhead; wheel scrolling moves the playhead; no horizontal scroll bar), ripple trims
for clip-edge drags with the selection tool, an audio row height factor and whether the view
stores zoom / scroll per timeline in `Project::viewState()` (only the Edit page's view does).
The registry zoom actions act on the visible view (the Edit page's skips itself while hidden).

The playhead lives in `Timeline` (`setPlayhead()` / `playheadChanged`); the widget follows it
automatically (except while the user scrubs). Nothing else is needed from playback.

## Actions connected here

`tool.select/trim/blade` (A/T/B), `timeline.snapping` (N), `timeline.linked_selection`
(Ctrl+Shift+L), `timeline.zoom_in/zoom_out/zoom_fit`, `timeline.add_transition` (Ctrl+T,
cross dissolve 1 s), `timeline.add_video_track/add_audio_track`, `trim.nudge_left/right` (, .),
`edit.split` (Ctrl+B), `edit.delete` (Backspace, lift), `edit.ripple_delete` (Delete /
Shift+Backspace; with nothing selected it closes the gap under the playhead on the target
video track), `edit.cut/copy/paste` (paste at the playhead onto the original tracks),
`edit.select_all/deselect_all`, `edit.insert/overwrite/place_on_top` (F9/F10/F12),
`clip.toggle_enabled` (D), `clip.link/unlink`, `clip.toggle_keyframe_editor/toggle_curve_editor/delete_keyframe/copy_keyframes/paste_keyframes` (KeyframeLanes), `mark.in/out/clip/selection/clear_in/clear_out/
clear_both/go_to_in/go_to_out/add_marker`, `edit.paste_insert` (Ctrl+Shift+V),
`edit.replace/fit_to_fill/append_at_end` (F11 / Shift+F11 / Shift+F12),
`trim.nudge_left_multi/right_multi` (Shift+, / Shift+.), `trim.trim_start/trim_end`
(Shift+[ / Shift+], leave a gap), `trim.ripple_trim_start/ripple_trim_end` (Ctrl+Shift+[ /
Ctrl+Shift+], ripple; editfix branch), `trim.extend_edit` (E), and the pure navigation actions
`transport.prev_edit/next_edit/go_to_start/go_to_end/prev_marker/next_marker`
(Up/Down/Home/End/Shift+Up/Shift+Down). **The playback module must not connect those
transport actions a second time** (they would step twice).

Added on the resolveedit branch (keys in docs/keymap-resolve-windows.md): `edit.ripple_cut`
(Ctrl+Shift+X), `edit.ripple_overwrite` (Shift+F10), `edit.delete_gaps`, `edit.join`
(Alt+\\), `edit.select_forward/backward(_all)` (Y family), `edit.paste_attributes` (Alt+V),
`trim.dynamic` (W), `trim.select_nearest_edit/clip` (V / Shift+V), `trim.swap_left/right`
(Ctrl+Shift+, / .), `clip.match_frame` (F), `clip.change_duration/change_speed` (Ctrl+D / R,
dialogs in TimelinePanel), `mark.modify_marker` (Ctrl+M), `mark.delete_marker` (Alt+M),
`timeline.target_video_n/target_audio_n/auto_select_video_n/auto_select_audio_n`. With no clip
selected and in + out marks set, Backspace lifts and Delete extracts the marked range on the
auto-select tracks; Ctrl+C / Ctrl+X copy / cut it. Ripple delete of clips makes the other
auto-select tracks follow where they have room. Mouse wheel: `EditBehaviour::wheelScheme`
(Resolve: Alt zooms, Ctrl scrolls time, Shift scales the track heights) and `zoomAnchor`.

Hooks the page / panel wire: `setSourceProvider()`, `setMatchFrameHandler()`,
`setHandlePrompt()`, `setMarkerEditor()`, `setPasteAttributesPrompt()`, `setPlayingProvider()`
(dynamic trim; default: AppContext::playback). Never call a `runEdit()`-based command inside
an open edit: a nested `cancelEdit()` cancels the outer edit too (use the data-level helpers,
e.g. `applyEditTrim()`).

Behaviour is specified item by item in `docs/behavior-spec.md`; conventions that differ
between editors are read from `EditBehaviour` (src/core), never inline constants.

Besides the clip selection (SelectionModel) the controller owns two exclusive selections:
the **edit point** (`selectedEdit()`: tail and/or head of touching clips; nudging trims or
rolls it, E extends it, Ctrl+T puts the transition there) and the **gap**
(`selectedGap()`: Backspace/Delete close it). Selecting clips clears both. A **transition**
is selected in `SelectionModel::transition()` (so the Inspector follows it):
`selectTransition()` clears everything else, Backspace/Delete call `deleteTransition()`.

`TimelineWidget::inspectRequested(clipId, InspectTarget)` (double-click on a transition, its
menu's "Open in Inspector", a click on a clip's fx badge) is wired by EditPage to show the
Inspector on the Transition / Effects tab. Clicking a clip makes it the current (last) clip of
the linked selection (`clickedWithLinks()`), so the Inspector shows the clicked part.

Source edits (`insertSource()`) use three-point editing: the timeline in mark (else the
playhead) is the destination, in + out set the length, an out mark alone backtimes.

## Keyframe and curve editors (spec 15.8 - 15.12)

Additive layer: `TimelineWidget` owns a `KeyframeLanes`. Before each layout rebuild it asks it
for extra lane heights per track (`TimelineLayout::setLaneHeights`); a row's `laneHeight` sits
below its clips (`clipRect()` excludes it, `laneRect()` returns it, hit-testing treats it as
empty track). The lane widgets are children of the viewport placed over `laneRect()` (cut below
the ruler) by `syncGeometry()` after every layout / scroll / zoom change; they take their own
mouse and key input, forward plain wheel events to the timeline, and paint their own playhead.
The only hook in the input state machine is the toggle-button click in `mousePressEvent()`.
Lane edits use the same protocol as timeline drags (copy origin, restore + re-apply per move,
one undo step). Selected keys live in `SelectionModel::keyframes()` (paths from
`core/KeyframeOps.h`). The lane widgets are thin subclasses of the shared views in
`src/ui/keyframes` (also used by the Keyframes panel) on a `KeyframeTimeMap` that goes through
`TimelineLayout`. A clip narrower than `KeyframeLanes::kMinUsableClipWidthPx` shows an "Open in
Keyframes Panel" button in its lane (the lane menus always have the entry):
`KeyframeLanes::openInPanelRequested(clipId, curves)`, wired by EditPage, which also follows
`editorOpenedByUser(curves)` to show the same view in the panel (spec 15.22). `TimelineWidget::viewChanged()`
(zoom / scroll) feeds the panel's Follow Timeline mode.

## Multicam clips (TimelineMulticam.cpp)

`chooseMulticamAngle()` (multicam viewer clicks and number keys) cuts at the playhead or
switches the segment under it, per the `multicam.mode_*` / `multicam.target_*` actions; one
undo step each, also while playing. The clip context menu offers Switch Multicam Clip Angle
(Video and Audio / Video / Audio), Open in Timeline and Flatten Multicam Clip
(`multicam.flatten`). The painter labels multicam segments "n  angle - clip" and their
thumbnails show the active angle (`nestedPreviewPath(multicamViewKey(...))`).

## Timelines, tabs and compound clips (v2)

The widget always shows `Project::timeline()`, the active timeline. On
`Project::activeTimelineAboutToChange` it stores its zoom / scroll in the project
(`setViewState()`), clears the selection and closes the keyframe lanes (clip ids belong to the
old timeline); on `activeTimelineChanged` it restores the new timeline's view (fit the first
time). Nested clips (`ClipKind::Nested`) are drawn in their own colour with thumbnails and a
waveform from `NestedPreview`; `makeMediaClips()` turns a timeline / compound clip item of the
media pool into Nested clips (refused when it would make the timeline contain itself), so drops
and F9/F10/F12 work unchanged. Actions: `timeline.new` (Ctrl+N), `timeline.duplicate`,
`timeline.delete`, `timeline.settings`, `timeline.next_tab`, `timeline.prev_tab`,
`timeline.close_tab`, `clip.new_compound`, `clip.open_in_timeline`, `clip.decompose`.

## Subtitle tracks, adjustment clips, generators (v2)
- `TimelineLayout::rebuild()` stacks subtitle rows above the video rows (ST(n) first); the
  divider stays between video and audio. Headers: "ST<n>" label without a source patch (target
  setters ignore subtitle refs), eye / lock / auto-select.
- `TimelinePainter`: adjustment clips slate with hatching, generators terracotta, captions
  sand with their text.
- `TimelineController::addSubtitleAtPlayhead()` (`timeline.add_subtitle`), `addTrack(Subtitle)`
  (`timeline.add_subtitle_track`); context menus of subtitle rows offer both.
- Drops (`TimelineDrop.cpp`): a subtitle media item becomes one caption per cue
  (`subtitles::captionsFromCues()`), overlapping cues on further subtitle tracks
  (`DropPlan::newSubtitleTracks`), never linked, always overwrite. Generator / adjustment
  descriptors use `effectactions::makeGeneratorClip()` like double-click.
## Retime controls (spec section 29)

`clip.retime_controls` (Ctrl+R) toggles the speed bar of the retime targets (selection, else
clips under the playhead on auto-select video tracks); the shown set is controller view state
(`retimeControls()`, cleared on timeline switch). `clip.freeze_frame` (Shift+R),
`clip.retime_curve` (Ctrl+Shift+R) and `clip.change_speed` (R, `ChangeSpeedDialog`) are
connected here too, and `clip.speed_increase` / `clip.speed_decrease` (no default keys,
`TimelineController::nudgeSelectionSpeed()`, spec 29.17: 10 percentage points through
`timelineedit::applyClipSpeed()`, one undo step). Handle drags follow the usual protocol (origin restore + `retimeops`
per move, one undo step); menus are non-modal `QMenu`s with object names for tests
(`TimelineWidget::retimeMenu()`). Tests: `tests/timeline/test_retimebar.cpp`.

## Coordinates and time

Timeline positions are frames (PLAN 5.1). `TimelineLayout` maps them:
`x = kHeaderWidth + frame * pixelsPerFrame - scrollX`. Edit points snap to the nearest frame
boundary (`nearestFrameAtX`), the playhead and the blade to the frame under the pointer
(`frameAtX`). Rows: V(n)..V1, a divider, A1..A(n); when everything fits, the divider sits in
the vertical middle (V1 and A1 meet at the centre). The horizontal scroll bar value is the
scroll in pixels.

## Painting and performance

- Only clips in the visible frame range are visited: `TimelineLayout::clipRange()` does two
  binary searches per track (clips are sorted and disjoint, so their ends are sorted too).
- Clips narrower than 4 px are a single `fillRect`; names, thumbnails and waveforms only appear
  when there is room. Elided names are cached per clip id; waveform peaks and line batches use
  member buffers, so steady-state painting allocates nothing besides QPainter internals.
- Selection is mirrored into a `QSet` for O(1) lookups (SelectionModel stores a vector).
- Benchmark (`test_timelinebench`, 2,000 clips, 1920x600 offscreen): 3.6 to 8.2 ms per full
  repaint from fit-all to max zoom; one live drag update (state copy + move) 3.4 ms.

## Animations (v2.2, TimelineAnimator.h, spec 36)

Display only: the model and the undo stack change instantly, hit tests use the model.
- `TimelineWidget::onTimelineChanged()` hands every *committed* change (no open edit, so live
  drags in any view stay 1:1) to `TimelineAnimator::modelChanged()` *before* the layout is
  rebuilt: the old rows give the old clip positions. One pass over the new clips with hash
  lookups into the previous state (`Timeline::plainSnapshot()`, which endEdit() needs anyway)
  records where each moved clip was drawn; new clips appear, removed ones become ghosts, a new
  clip that forms a through edit with the old clip it came from is a blade cut (flash + halves
  apart). Changes during an open edit mark the animator stale; the undo stack's indexChanged()
  and `finishDrag()` resync it without animating.
- `TimelinePaintInput::anim` is set only while something runs: the painter then asks
  `displayClip()` for each clip's rect and opacity, widens the visible clip range by
  `maxShiftFrames()` and draws ghosts, flashes, the pulsed edit point bracket, the snap fade and
  the gliding playhead. With animations idle or off (`uianim::enabled()`) the old paths run
  unchanged and the widget makes no animator calls at all.
- Time: `uianim::nowMs()` frozen per paint by `tick()`; a precise timer at the screen refresh
  rate repaints the viewport while anything runs. Durations and easing: `ui/common/UiAnimation.h`.
- Test: tests/timeline/test_timelineanimation.cpp (test clock; KK_ANIMATIONS=1 since offscreen
  defaults to off; KK_ANIMATION_SHOTS=<folder> writes demo frames).

## Input state machine (TimelineInput.cpp)

```
Idle --press--> [area/tool decides]
  ruler                      -> Scrub (immediately; snaps to edges/marks when snapping is on)
  header button              -> toggle (one undo step), back to Idle
  header bottom edge         -> ResizeTrack
  middle button              -> Pan
  blade tool on a clip       -> split clip + linked clips at the pointer (one undo step)
  transition block (not blade) -> select the transition, back to Idle (double-click: inspect)
  fx badge                   -> select the clip, inspectRequested(Effects)
  select tool:  cut centre   -> select edit (both sides), Pending(Roll)
                clip edge    -> select edit point, Pending(TrimHead/TrimTail, leaves gaps)
                clip body    -> click selection, Pending(Move)   (Alt: one side; drag duplicates)
                empty        -> select gap, Pending(RubberBand)
  trim tool:    cut centre   -> select edit (both sides), Pending(Roll)   (+-kRollZonePx)
                clip edge    -> select edit point, Pending(TrimHead/TrimTail, ripple)
                name band    -> Pending(Slide)                    (Alt held: Slip)
                clip body    -> Pending(Slip)                     (Alt held: Slide)
                empty        -> select gap, Pending(RubberBand)
Pending --moved past threshold--> startDrag(mode): copy origin, beginEdit()
Drag mode --move--> updateDrag(): restore origin, apply op with the total delta,
                    notifyChanged(); auto-scroll near the left/right edge
          --release--> finishDrag(): endEdit()  (one undo step)
          --Esc-------> cancelDrag(): cancelEdit()
```
Re-applying the operation from the press-time state on every move keeps overwrite semantics
exact while dragging back and forth. With linked selection on, linked clips move, trim
(same edge on the same frame), roll (same cut), slip and slide together, and blade and the
commands include them.

Snapping: candidates from `editops::snapCandidates()` excluding the dragged clips, tolerance
8 px; a move snaps whichever edge of the grabbed clip is closer; the snap line is drawn in
yellow.

## Drag and drop (TimelineDrop.cpp)

MIME types from `ui/common/DragMime.h`. `planDrop()` computes a `DropPlan` from the payload and
the position; the same plan draws the ghost preview and performs the drop (one undo step).
- media items / source range: video on the video row under the mouse (or V of the same index),
  audio on the audio track of the same index, linked; several items are placed one after
  another. Overwrite by default, Ctrl inserts.
- video/audio effect: appended to the clip under the mouse (matching track type).
- transition: head or tail of the clip under the mouse (nearest half); a tail touching the next
  clip becomes that clip's `transitionIn`, centred on the cut; 1 s.
- generator / title: a new 5 s clip on the video track under the mouse (V1 otherwise).
- empty space of the track area (Resolve): above the top video track a new video track is
  created for the drop, below the last audio track a new audio track; the other half of a
  linked clip goes to the same index, created when missing (V3 pairs with A3); a title or
  generator only goes above. The gap between V1 and A1 counts as the nearer row. The ghost is
  drawn in the band where the new track will appear. Ruler and headers refuse drops.
- files from a file manager (`platform::droppedLocalFiles()`): imported into the media pool
  (one undo step, the usual report for unsupported files) from the event loop after the drop,
  then placed like a media pool drag at the drop position (a second step).
Every `DragEnter` / `DragMove` is answered explicitly (see src/ui/common/readme.md, "Drag and
drop", for the Wayland rules); `kk.ui.dnd` logs them. Tests: `test_timelinedrop.cpp`.

## How to extend

**A new tool:**
1. Add the action to `StandardActions.cpp` (group `trim`, checkable, in the tools
   `QActionGroup`) and a key to `resources/keymaps/resolve-windows.json`.
2. Add a value to `TimelineTool` and map its action in `TimelineController::connectActions()`
   and `setTool()`.
3. In `TimelineWidget::mousePressEvent()` decide the `DragMode` for the new tool (add one to
   `DragMode` if needed); set up the drag in `startDrag()` (which clips, snap exclusions, edit
   label) and apply the operation in a new `updateXxx()` called from `updateDrag()`, always
   starting with `restoreOrigin()`.
4. Give it a cursor in `updateHover()`, and a QTest gesture test in `test_timelinewidget.cpp`.

**A new command:** add a slot to `TimelineController` that wraps the change in `runEdit()`
(returns whether anything changed), connect its action in `connectActions()`, test it in
`test_timelinecontroller.cpp`.

**A new edit operation:** implement it in `core/EditOps` first (pure, invariant-checked,
tested in `tests/core/`), then call it from the controller or a drag mode.

**Something new on clips:** draw it in `TimelinePainter::paintClip()`; keep it out of the
hot path unless the clip is wide enough, no QString formatting per clip, reuse member buffers.

## Clip colours and flags (bins branch)
`TimelinePainter::clipColor(clip, type, item)`: the clip's own `ClipData::label`, else the clip
colour of its media pool item, else the colour by kind. Flags of the item are drawn as small
squares at the right of the name band. The clip context menu adds "Clip Color" / "Flags"
(`ClipColorMenu.h`; flags edit the media pool item) and Find in Media Pool
(`timeline.find_in_media_pool`, handled by the Edit page).

## Level lines, fade handles, stacked timelines (spec section 31)
- `LevelLines.h`: pure geometry of the audio level line (dB <-> y over the waveform area, key
  and line hit-testing) and of the fade handles. `TimelinePainter::paintVolumeLine()` draws
  the exact curve + key diamonds, `paintFades()` the wedges and (hovered clip) the handles.
- `TimelineLevelsInput.cpp`: selection-tool presses on fade handles (`DragMode::Fade`), the
  line (`LevelLine`, Shift = fine, Alt+click adds a key) and keys (`LevelKey`); edits are
  `core/LevelOps.h`, one undo step per gesture; right-click on a key = Delete Keyframe.
  `edit.delete` / `edit.ripple_delete` delete selected keys when no clip is selected.
- Clip context menu: `clip.attributes` (Clip Attributes dialog, src/ui/dialogs).
- Stacked timelines (`view.stacked_timelines`, `TimelinePanel::setStackedTimelines()`): a
  vertical splitter with two slots; the editor group (TimelineTabs + TimelineWidget) sits in
  one, `StackedTimelinePane` (own tab list, "+" menu, read-only `StackedTimelineView` with own
  zoom / scroll and the inactive timeline's own playhead) in the other. A click in the pane
  calls `activateStackedPane()`: the pane's timeline becomes active, tab lists swap
  (`Project::setTabs()`), the editor moves into the clicked slot. Copies between them use
  MIME `mime::TimelineClips` ({timeline, clips, grabFrames}): the pane starts a QDrag from a
  clip, the editor starts one when a move leaves the view vertically
  (`TimelineWidget::setClipDragOut()`); both end in `TimelineController::copyClipsFrom()`
  (TimelineStackedDrop.cpp plans the editor's ghost).
