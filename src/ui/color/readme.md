# src/ui/color — Color page widgets

The Color page's panels (docs/color-page.md, spec 44b). The page itself, which lays them out,
is `src/app/pages/ColorPage`. Widgets never edit the project directly: every change goes
through ColorSession (one undo step per command or gesture).

## Key classes
- **ColorSession**: the grade being edited (the clip under the playhead, a clip picked in the
  strip, or the timeline grade), the selected node and window, the strip's selected clips and
  the copied grade; every command through `Project::setClipGrade()` / `setTimelineGrade()`.
  Values are set at the playhead (a key when keyframed); drags are one step via
  `beginGesture()` / `endGesture()`; every commit inside a drag still notifies the timeline
  (`Project::notifyLiveGrade()`), so the viewer follows the mouse (spec 44b.31).
- **ColorFrameSource**: the ungraded frame at the playhead and the clip thumbnails (renders
  through `PlaybackEngine::renderImage()`), node outputs, the highlight and eyedropper samples
  (graded on the CPU with `render::applyGrade()`).
- **ColorNodeEditor**: the node graph (tiles with thumbnails, mixers, Key Mixer, Splitter / Combiner, Compound and Alpha Output nodes, picture and key links, gestures, rubber band, open compound level).
- **ColorWidgets**: `ColorWheel`, `PrimaryBars`, `PrimariesPanel`, `GradeCurveEditor`,
  `ColorCurvesPanel`.
- **ColorPalettes**: `QualifierPanel` (with `QualifierRangeBar`), `WindowsPanel`, `LutBrowser`,
  `KeyPanel`.
- **ColorMatchPanel**: the Color Match palette (chart type, gammas, colour space, white point, Match; chart outline over the viewer by `ColorViewerOverlay`); maths in `core/ColorMatch`. Shot Match is `ColorClipStrip::shotMatch()`.
- **QualifierPick**: HSL ranges from picked colours (pick, add, remove).
- **ColorViewerOverlay**: window outlines and handles, the eyedropper rectangle, the highlight
  picture and the split wipe over the viewer.
- **ColorSplitScreen**: View > Split Screen over the viewer (Selected Clips grid, Timeline strips).
- **ColorLightbox**: View > Lightbox, the grid of every clip's graded thumbnail (`ColorLightboxGrid`).
- **ColorBrowsers**: `ColorGallery` (album list, stills, memories), `ColorClipStrip`, `ColorMiniTimeline`.
