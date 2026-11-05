# src/ui/keyframes — keyframe views and the Keyframes panel

Everything that shows and edits keyframes on a time axis. Used by the timeline's inline lanes
(src/ui/timeline: KeyframeTrackWidget, CurveEditorWidget) and by the Keyframes panel (this
module). Depends on `core`, `ui/common` and `ui/inspector` (ValueField, KeyframeButton,
ParamAccess); never on `ui/timeline`.

## Files

| File | What |
|---|---|
| `KeyframeTimeMap.h` | interface timeline frame <-> widget x; `LinearTimeMap` (panel, tests) |
| `KeyframeViewBase.h` | base of every view: shown clips, label / axis column, vertical window, selection, drag / undo protocol, snapping, key menu, painting helpers, Delete / Ctrl+C / Ctrl+V |
| `DopeSheetView.h` | rows of diamonds (`KeyRow`: master, item, group, parameter); click / rubber band / time drag |
| `CurveView.h` | overlaid curves (`CurveInfo`), Normalised / Absolute value axis, value zoom and pan, fit all / fit selection, key and Bezier handle drags, double-click adds a key |
| `KeyframeCommands.h` | delete / copy / paste / interpolation / flatten on the selected keys, one undo step each; the one keyframe clipboard |
| `KeyframeGrid.h` | grid steps (values, frames / seconds) |
| `KeyframeRuler.h` | panel ruler: timecodes, clip spans, playhead; drag scrubs the playhead, snapping like the timeline ruler (spec 11.9) |
| `KeyframeItemModel.h` | pure: selection -> items (linked pair = one item) -> rows grouped like the Inspector |
| `KeyframeParamTree.h` | panel tree: curve check box, prev / diamond / next, inline value (ValueField) |
| `KeyframesPanel.h` | the panel: tool row, tree, ruler, dope sheet or curves (`setView()`, one at a time in a `QStackedWidget`; spec 15.22), time scroll bar, view modes, persistence; header gestures: the labelled Float / Dock button, a double-click on the header floats / docks, dragging the docked header out of the panel tears it off (`tearOffRequested()`), dragging a floating panel's header moves its window (`QWindow::startSystemMove()`, the only way on Wayland) |

## Shared code vs. specific code

The inline lanes and the panel run the same editing code: selection, drags (copy origin,
restore + re-apply per move, one undo step), snapping, the key menu, curve sampling, hit tests
and handle maths are in `KeyframeViewBase` / `DopeSheetView` / `CurveView` on top of the pure
functions of `core/KeyframeOps.h`. What differs is only geometry and chrome:

| | inline lane (src/ui/timeline) | Keyframes panel |
|---|---|---|
| time map | `KeyframeLanes::LayoutTimeMap` (TimelineLayout) | `LinearTimeMap` (own zoom / scroll) |
| left column | track header width: row labels / parameter list + tool bar | value axis (64 px), labels in the tree |
| vertical window | cut under the timeline ruler | dope sheet scrolled with the tree |
| clips | one clip | the selected items (linked pairs, several items) |
| value axis | Normalised | Normalised or Absolute (toggle) |

## Panel views, colours, snapping

Both views stay alive and share `m_map`, the selection and the tree: switching only changes the
stack's page and which of Normalised / Fit Selection show. `view.keyframes_panel_toggle_view` and the
lane entry points are wired in `EditPage`. The panel's views call `setPanelColors(true)` and paint with
`SchemeColors::keyRows`, `keyRowStripe` and `keyRowsOutside`; the inline lanes keep their own shades.

Snap targets have one source: `editops::snapCandidates()`, with
`keyframeops::withKeySnapTargets()` adding keys. The panel's ruler and the timeline's scrub use it for
the playhead; `KeyframeViewBase::beginDragEdit()` for key drags, leaving out the selected keys.

## Data flow

`Timeline::changed` / `SelectionModel::changed` -> `KeyframesPanel::rebuild()` builds the model
(cheap: descriptors of the shown clips), passes rows to the tree and the dope sheet and curves to
the curve view. The tree rebuilds widgets only when the rows changed; otherwise it refreshes
values and diamond states (O(rows)). Playhead changes only repaint and refresh the tree.

## How to extend

**A new group of parameters** (e.g. a new clip property group): register its descriptor in
core (EffectRegistry) and add it to `keyframeitems::build()` next to Transform / Cropping /
Audio; tree, dope sheet and curves pick it up.

**A new view on keyframes** (e.g. a speed graph): derive from `KeyframeViewBase`, take a
`KeyframeTimeMap`, use `beginDragEdit()` / `restoreDragOrigin()` / `endDragEdit()` for drags and
`keyframecommands` for commands.

## Masks
Video clips list their masks after Cropping in one "Masks" group (`appendMasksGroup()` in
`KeyframeItemModel.cpp`): every animatable mask parameter labelled "<mask>: <parameter>" and,
for point masks, a "<mask>: Shape" row (path `mask/<id>/shape`). The shape row has keys but no
value field (shapes are edited in the viewer); vertical drags in the curve editor leave shape
keys alone (`keyframeops::offsetKeyValues()` skips them).
