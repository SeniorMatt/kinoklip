# src/ui/cut — the Cut page's own widgets

Widgets and commands used only by the Cut page (`src/app/pages/CutPage.*`), the v2.5 page for
fast single-timeline editing (docs/behavior-spec.md section 35). The pure logic lives in core:
`core/CutEdits.h` (the smart edits, fast review speed) and `core/SourceTape.h` (the source tape
and its frame / range mapping). Nothing here owns model state: everything edits the shared
Project through the Edit page's `TimelineController`.

| File | What |
|---|---|
| `CutViewer.h` | the single large viewer: a stack of three `ViewerPanel`s (Source Clip, Source Tape with the `SourceTapeBar`, Timeline) switched by the `cut.viewer_*` actions; builds the tape from the media pool's visible items; `sourceMaterial()` = what the smart edits place |
| `SourceTapeBar.h` | the clip-boundary bar under the tape viewer (blocks per clip, in / out band, position; click / drag seeks) |
| `CutOverview.h` | the upper half of the dual timeline: the whole timeline fitted to the width, the lower timeline's range as a band, the playhead with its head in the ruler strip; click / drag moves the playhead (a click glides the playhead line, `uianim::PlayheadGlide`), with scrub audio only from the ruler strip (spec 1.28) |
| `CutToolBar.h` | the tool bar above the dual timeline: six labelled smart edit buttons, split / ripple delete / snapping, the quick dissolve button, Fast Review, a message line, zoom |
| `CutEditor.h` | runs a `CutEdit` (smart insert, append, ripple overwrite, close up, place on top, source overwrite) with the material of the viewer: builds the clip groups with `TimelineController::makeMediaClips()`, applies `cutedits::*` in one undo step, selects the new clips and moves the playhead to their end; `handleSourceEdit()` maps the Edit page's F9 / Shift+F10 / F12 / Shift+F12 onto the Cut versions |
| `FastReview.h` | Fast Review on the program engine: per-shot rate from `cutedits::fastReviewRate()`, ends on any other transport command |

## Data flow
```
media pool (visible ids) --> CutViewer::setTapeItems() --> SourceTape --> tape ViewerPanel
viewer mode + marks ------> CutViewer::sourceMaterial() --> CutEditor::run(edit)
CutEditor --> TimelineController::makeMediaClips() --> cutedits::<edit>(mutableData) --> one undo step
Timeline::playheadChanged --> lower TimelineWidget (centred playhead) --viewChanged--> CutOverview band
```

## Lower timeline
A plain `TimelineWidget` (src/ui/timeline) with `TimelineViewOptions`: `centredPlayhead`
(scrolls under a fixed playhead; wheel scrolling moves the playhead), `rippleEdgeTrims` (edge drags
ripple with the selection tool, the Cut page default), `audioHeightScale` 0.55 (compact audio) and
`keepsViewState` off (the Edit page's view keeps the per-timeline zoom). Everything else (moves,
trims, roll at the cut centre, blade tool, drops, menus, keyframe lanes) is the Edit page code.

## Shortcuts
Only through the ActionRegistry: `cut.*` actions (StandardActions.cpp, category "cut", the Edit
menu's "Cut Page" submenu), enabled by the page while it is shown. Default keys come from
`scripts/keymap-resolve-windows.py` ("Cut page" section). Edit page keys keep working: Q switches
the viewer between the last source mode and the timeline, F9 / F12 / Shift+F10 / Shift+F12 run the
smart edits (`TimelineController::setSourceEditHandler()`), Ctrl+\ splits, Ctrl+T dissolves.

## Colours
Everything painted here uses `kk::schemeColors()` (clip colours via
`TimelinePainter::clipColor()`), and repaints on `SchemeColorsNotifier::changed`.

## How to extend
**Add a smart edit button:**
1. Write the edit as a pure function in `core/CutEdits.h` taking `SourceGroups` and returning a
   `cutedits::Result`; test it in `tests/core/test_cutedits.cpp`.
2. Add a `CutEdit` value, its label and a `kEditActions` row in `CutEditor.cpp`, and the case in
   `CutEditor::run()`.
3. Register the action (`cut.<name>`, category "cut") in `ui/common/StandardActions.cpp`, add a
   row to the "Cut page" section of `scripts/keymap-resolve-windows.py` and run the script.
4. Add it to `kSmartEdits` in `CutToolBar.cpp`, and a spec item in docs/behavior-spec.md 35.

**Add a viewer mode:** add a `CutViewer::Mode`, a `cut.viewer_*` action in the exclusive group
(StandardActions.cpp) and a `kModeActions` row, put its widget into the stack and extend
`sourceMaterial()`.
