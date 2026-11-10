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
- **PatchMatch.h**: the matching steps of the PointTracker (exhaustive correlation search,
  Gauss-Newton refinement) shared with the PlanarTracker.
- **Homography**: the planar tracker's maths as small free functions: `homographyFromPoints()`
  (normalised DLT, Jacobi eigen-decomposition of the 9 x 9 system, degenerate input refused),
  `estimateHomography()` (RANSAC with a fixed seed, then refit on the inliers),
  `similarityFromPoints()` (move / turn / scale best fit), quad helpers.
- **PlanarTracker**: follows a four-corner region: a grid of patches warped by the previous
  homography, matched with the PatchMatch steps, fitted with `estimateHomography()`, refined by a
  second stricter pass; same `reset()` / `track()` shape as the PointTracker.
- **Stabilizer** / **StabilizeJob**: Stabilization (spec 58). `estimateFrameMotion()` runs a PlanarTracker over the whole picture between two frames and fits the model of the chosen Mode to the agreeing patches (`PlanarResult::inlierFrom/inlierTo`); `stabilizeAnalyse()` chains the pairs into the camera path (`stabilize::Data`); `planStabilizeJob()` / `runStabilizeJob()` decide which source frames a clip shows and decode them (VideoDecoder); `StabilizeRunner` runs a job on a worker thread for the Inspector.
- **EffectTrackJob**: the Track buttons of the tracking effects (spec 70): plans a `TrackJob` and the private timeline copy whose frames are tracked (`planEffectTrack()`), turns the samples into a `core/EffectTrack.h` table keyed by source time and stores it in the effect.
- **TrackerRunner**: runs a point job or (`TrackJob::planar`) a planar job off the UI thread.
- **TrackStore**: writes / clears a track in a Tracker or Planar Tracker node (keyframes).
- **TrackLink**: "Connect to Tracker": copies a track's centre into another node's Center keys.

## How to extend
A new kind of track (say an affine tracker, or tracking from feature points) is a class with the
PointTracker's `reset()` / `track()` shape; give `TrackJob` the extra start data and
`TrackSample` the extra result values, branch once in `TrackerRunner` (see `runPlanar()`), store
the result in `TrackStore`, and add the node type to `trackSources()` / `trackCentre()` in
`TrackLink` so other nodes can follow it. The maths stays in free functions with synthetic tests.

Tests: tests/compositing/test_pointtracker.cpp (synthetic moving textures, known sub-pixel
offsets, noise, loss), test_homography.cpp (DLT, RANSAC, similarity, degenerate input),
test_planartracker.cpp (a plane moved by known homographies), test_planartrackernode.cpp (node,
render, runner, save / load, track links), test_stabilizer.cpp and test_stabilizejob.cpp (synthetic shaky footage, `ShakyFootage.h`).
