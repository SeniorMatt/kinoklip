# src/ui/timeline — timeline editor

**Status: placeholder from the foundation agent; the timeline agent owns this module.**

`TimelinePanel` = `PanelFrame` titled with the timeline name, a `QToolBar` bound to the registry
actions (`tool.select/trim/blade`, `timeline.snapping/linked_selection`,
`edit.insert/overwrite/place_on_top`, `timeline.zoom_*`) and a read-only `TimelinePreview`
(ruler, track headers, clip rectangles, playhead; clicking the ruler moves the playhead).

## How to extend
Replace `TimelinePreview` with the interactive `TimelineWidget`: hit-testing, tools, snapping
(`editops::snapCandidates/snapToNearest`), and edits through
`project->beginEdit()` → `editops::*` on `timeline()->mutableData()` → `notifyChanged()` (live)
→ `endEdit()` (see `src/core/README.md`). Connect the `tool.*`, `edit.*`, `timeline.*`,
`clip.*` and `mark.*` actions listed in `src/ui/common/README.md`.
