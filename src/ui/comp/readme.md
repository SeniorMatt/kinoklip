# src/ui/comp — Compositing page widgets

The node compositor's panels (docs/compositing-page.md). The page itself, which lays them out,
is `src/app/pages/CompositingPage`.

## Key classes
- **CompAdapter** (`compadapter::`): the only door to the core — the node catalogue as the UI
  needs it (from `core/NodeRegistry`), the graph operations (`core/CompOps`), the on-viewer
  control keys and the undoable `Project::setClipComposition()` calls.
- **CompSession**: the open clip, its graph (stored, or the unsaved default), node selection,
  viewer nodes, and every editing command as one undo step. Widgets only map input to it.
- **NodeEditor** (+ `NodeEditorPaint.cpp`): custom-painted canvas (why not QGraphicsView: see
  the class doc). Gestures, keys through the comp.* actions, drag and drop; panel-local Find
  (Ctrl+F) and Paste Settings (Ctrl+Shift+V); tile colours, tile pictures (rendered through a
  page-supplied source, throttled), tooltip and Replace With (spec 93).
- **CompViewer** + **CompViewerOverlay** + **ViewerFx**: the two viewers (surface, guides, node drops, A/B buffers, channel / checker / split tools with viewer-local keys, lock, clear) and the
  on-viewer handles of the selected node.
- **CompInspector**: the Edit page parameter rows on a stand-in clip whose one effect carries the
  node's ParamSet (see the class doc); nodes with a Center also get the "Follow Track" row
  (`compositing/tracking/TrackLink`).
- **CompTransport** / **CompTimeRuler**: ruler and play / stop over the clip's frames.
- **TrackerPanel**: the Tracker and Planar Tracker nodes' Track Backward / Stop / Track Forward buttons, progress and
  status in the Inspector; runs `compositing/tracking/TrackerRunner` on pictures from
  `PlaybackEngine::renderCompositionNode()` and stores the result as one undo step.
- **NodeLibraryPanel**, **SelectNodePopup**: the Nodes library and the Shift+Space list.

## Data flow
Input -> CompSession command -> `compadapter` (CompOps on a copy) ->
`Project::setClipComposition()` -> `Timeline::changed()` -> CompSession reloads -> widgets
repaint. Gestures (slider drags, viewer handles) open a `Project::beginEdit()` so they are one
step. UI state (selection, viewer nodes, view) never makes a step; the view state is written
into the composition with the next edit.

## How to extend
- **New node type**: add it to `core/NodeRegistry`; it appears in the library, the Select Node
  list and the context menu. For a tool bar button add its id to `compadapter::toolbarTypes()`.
  On-viewer handles come from `NodeDescriptor::controls` (`compadapter::viewerControls()`).
- **New node editor command**: add a `comp.*` row to `kActions` (StandardActions.cpp) and to
  `scripts/keymap-resolve-windows.py`, run the script, and connect it in
  `CompositingPage::connectActions()` to a CompSession method. Its key works while the node
  editor has focus (scope "widget:node_editor": it may share a key with an Edit page command
  without Keyboard Customization calling it a conflict). The Edit page timeline commands are
  disabled while the page is shown (`CompositingPage::suspendedCommandIds()`); a command that
  must stay available there goes into its keep list.
- **Viewer pictures**: `PlaybackEngine::renderCompositionNode()` (the engine's media table,
  proxies and quality); never build a `RenderRequest` by hand in the page.
