# src/ui/color — Color page widgets

The minimal Color page's panels (docs/color-page.md, spec 44b). The page itself, which lays
them out, is `src/app/pages/ColorPage`.

## Key classes
- **ColorSession**: the grade being edited (the clip under the playhead, or the timeline
  grade), the selected node, and every command as one undo step through
  `Project::setClipGrade()` / `setTimelineGrade()`; values are set at the playhead (a key when
  the value is keyframed). Gestures (wheel drags, curve drags) are one step via
  `beginGesture()` / `endGesture()`. Also bypass all, grab / apply stills, copy grade.
- **ColorWheel** / **PrimariesPanel**: the Lift / Gamma / Gain / Offset wheels (luma-neutral
  colour balance; the bar below sets the master) and the primaries fields.
- **GradeCurveEditor**: the nine curves of the selected node.
- **NodeList**: the nodes in evaluation order, with a check box to bypass each corrector.

The full page (node editor, palettes) is built on ColorSession; widgets never edit the
project directly.
