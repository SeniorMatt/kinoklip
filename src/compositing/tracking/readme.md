# `src/compositing/tracking` — motion tracking maths

Pure maths for the Compositing page's Tracker node; no widgets, no threads, no project types
(depends on QImage only to read a picture). Namespace `kk::tracking`.

## Key classes
- **PointTracker**: follows one point from frame to frame. Normalised cross-correlation
  template matching over an image pyramid (coarse search in the whole search box, then a few
  pixels around the scaled-up result per finer level), then Gauss-Newton refinement of the
  position on bicubic samples for sub-pixel accuracy. Invariant to brightness and contrast
  changes. The pattern is kept from the start frame and re-taken from the current frame when the
  match gets weak (`refreshBelow`); a match below `minCorrelation` is a loss.
- **GrayImage** / `grayFromImage()`: the grey picture it works on.

## How to extend
Planar tracking (a plane's four corners) would be a sibling class with the same
reset() / track() shape that follows a homography instead of a position; the Tracker node's
UI (src/ui/comp/TrackerRunner) only needs the new class and a result with more values.

Tests: tests/compositing/test_pointtracker.cpp (synthetic moving textures, known sub-pixel
offsets, noise, loss).
