# `src/render` — compositor and audio mixer

Turns a `TimelineData` snapshot into pictures (GPU compositor, OpenGL 3.3 core, with a QPainter
fallback) and sound (`AudioMixer`). Namespace `kk` for the public API, `kk::render` for internals;
logging category `kk.render`. Depends on `core` and `media`; never on widgets.

## Public API

| Header | What |
|---|---|
| `Compositor.h` | `Compositor`: render thread + GL context; `render()`, `renderAsync()`, `renderImage()`, `setMediaPaths()`, `isGpu()` |
| `RenderTypes.h` | `Quality` (Full/Half/Quarter), `MediaPathMap` + `makeMediaPathMap()`, `RenderOutput` flags, `RenderRequest`, `RenderedFrame` / `RenderedFramePtr` |
| `AudioMixer.h` | `AudioMixer::mix(timeline, startSample48k, frames, out)` — 48 kHz interleaved stereo float |
| `RenderFrameCommand.h` | `runRenderFrame()` behind `kinoklip --render-frame <project.kkproj> <frame> <out.png>` |

### Viewer / playback (zero copy)

```cpp
kk::Compositor compositor;                       // GUI thread; blocks ~50 ms for GL init
compositor.setMediaPaths(kk::makeMediaPathMap(project->mediaPool()->state()));
auto future = compositor.renderAsync({timeline->snapshot(), {}, frame, viewerSize,
                                      kk::Quality::Half, kk::OutputTexture});
kk::RenderedFramePtr f = future.get();           // or poll future.wait_for(0) from a timer
// In the viewer's QOpenGLWidget::paintGL() (its context shares with the global share context):
glWaitSync(static_cast<GLsync>(f->fence), 0, GL_TIMEOUT_IGNORED);   // GPU-side wait, no stall
glBindTexture(GL_TEXTURE_2D, f->texture);        // RGBA8, premultiplied, row 0 = TOP
// draw a quad with uv.y = 0 at the top (same as uploading a QImage); keep `f` alive until the
// next frame replaces it — releasing the last RenderedFramePtr recycles the texture.
```
`compositor.isGpu() == false` (no GL 3.3, `KINOKLIP_RENDER=cpu`): request `OutputImage` and
show `f->image` with the raster path instead (`texture` is 0).

### Export (readback)

```cpp
std::deque<std::future<kk::RenderedFramePtr>> inFlight;
for (int64_t f = in; f < out; ++f) {
    inFlight.push_back(compositor.renderAsync({snapshot, media, f, {}, kk::Quality::Full,
                                               kk::OutputImage}));
    if (inFlight.size() == 3) { encode(inFlight.front().get()->image); inFlight.pop_front(); }
}
// drain the rest. Image = QImage::Format_ARGB32_Premultiplied; empty areas are transparent
// black, so "over black" = just use the RGB (alpha can be ignored).
```
Keeping 2-3 requests queued lets the PBO readback of frame N overlap rendering of frame N+1
(~20 % faster than one at a time, see the numbers below).

### Audio

```cpp
kk::AudioMixer mixer;                            // one per consumer thread (not thread-safe)
mixer.setMediaPaths(paths);
mixer.mix(*snapshot, kk::framesToSamples48k(frame, fps), 1024, buffer);   // 2048 floats
```

## Pipeline

```
 RenderRequest ─► queue ─► render thread (owns GL context, decoders, all GL objects)
                          │
   1. evaluateTrack() per enabled video track (V1 first)  ── TimelineEval.h (pure, shared
      → single clip | transition span (A, B, progress)       with CPU path + AudioMixer)
   2. DecoderPool.acquire(path, t) per visible media clip; decodeFrame() in parallel
      (QThreadPool, one decoder per use) → VideoFrame planes. Clips playing forward one frame
      at a time then start decoding frame + 1 on the pool (decode-ahead, overlaps steps 3-4
      and the wait for the next request; the next decodeFrame() returns the cached frame)
   3. per track, into the RGBA16F accumulator (premultiplied "over"):
        clip ──► source: YUV planes (persistent per-decoder textures, glTexSubImage2D straight
        │                from decoder memory) | title texture (cached) | solid colour
        ├─ no effects/masks: ONE pass  source → crop → YUV→RGB → transform → opacity → accumulator
        └─ effects or masks: source → crop → layer (RGBA16F, layer resolution, optional margin)
                       → effect passes (ping-pong, pooled targets)
                       → mask pass (GlMasks: matte × layer)
                       → transform → opacity → accumulator
        transition: A and B each drawn (as above) into an output-size temp,
                    transition shader (A, B, progress) → accumulator
   4. accumulator → RGBA8 output slot (texture + GLsync fence for the viewer)
                 └► glReadPixels into a PBO → (next request / idle) map → QImage
```

### Conventions

* **Orientation**: every texture/framebuffer stores row 0 = top of the picture; NDC y = −1 is
  the top. Nothing is flipped anywhere and `glReadPixels` yields QImage row order.
* **Colour**: premultiplied alpha everywhere; compositing happens on gamma-encoded values (like
  most NLEs' default, and what users expect from a dissolve); intermediates are RGBA16F so
  multi-pass effects do not band; output is RGBA8. YUV → RGB honours `VideoFrame::matrix`
  (BT.601/709/2020) and `range` (limited/full) at 8 or 10 bits (`YuvMath.h`).
* **Geometry**: a media clip at zoom 1 is *fitted* into the timeline frame (aspect kept, after
  SAR and display rotation); titles and solids cover the frame. `posX/posY` are timeline pixels
  from the frame centre, scaled to the output size. The anchor is the pivot of zoom and rotation
  and is placed where the position puts it (changing only the anchor never moves an unscaled,
  unrotated picture). Rotation is clockwise. Crop values are source pixels (timeline pixels for
  generated clips). See `clipTransform()`.
* **Effect units**: pixel parameters (blur radius, shadow distance, mosaic block) are timeline
  pixels; effects multiply by `EffectInput::scale` so the look does not change with output size
  or preview quality. Blur sigma = radius / 2.
* **Transitions** (PLAN §5.4, `TransitionSpan`): `transitionIn` on B with an adjacent A is
  centred on the cut, window `[cut − d/2, cut − d/2 + d)`; A plays on into its handle, B starts
  early (decoder clamps = freeze on first/last frame without handles). No neighbour → fade
  from/to transparent over the clip's first/last d frames. B's `transitionIn` wins over A's
  `transitionOut` at the same cut. Progress `(f − start + 1) / (d + 1)`. Unknown transition
  ids render as a cross dissolve. Wipe/push take the `direction` choice; a numeric `angle`
  parameter (degrees, 0 = right, clockwise), if present, overrides it.
* **Titles**: rasterised with QPainter into a tightly cropped image (outline = stroked
  `QPainterPath` under the fill, shadow, background box, alignment, size/tracking/line
  spacing), uploaded once and cached by a hash of every style value + raster scale
  (`TitleStyle`), so a static title costs one textured quad per frame. Animated title
  parameters re-rasterise on each frame they change. Zoomed titles are rasterised at a
  quantised zoom (≤ 4×) to stay sharp.

### Masks (`GlMasks`)

`ClipData::masks` (core/Mask.h) are applied to the clip's layer after its effects and before
its transform, so they move with the clip and the clip opacity / transitions apply on top.
* **Rectangle / ellipse**: analytic in the combine shader: signed distance of the rotated
  shape (exact for the rectangle, a first-order estimate f/|∇f| for the ellipse, exact for
  circles), shifted by the expansion, through `1 − smoothstep(−featherIn, +featherOut, d)` (at
  least one pixel wide = antialiasing).
* **Point paths**: flattened on the CPU (adaptive cubic subdivision, 0.25 layer px), offset by
  expand + (featherOut − featherIn) / 2 (miter joins), filled with ONE triangle fan whose blend
  function `ONE_MINUS_DST_COLOR, ZERO` toggles the destination (even-odd fill, exact 0 / 1, no
  stencil buffer) into an RGBA8 coverage target, then blurred with `GlKit::gaussianBlur()`
  (σ = (in + out) / 3.76 — the smoothstep's slope at the edge — at least 0.5 px). Feathered
  coverage is rasterised and blurred at 1/2, 1/4 or 1/8 resolution while σ stays ≥ 1.5 px
  there; the combine pass upsamples it with bilinear filtering.
* **Combine**: one fullscreen pass evaluates up to 8 masks (≤ 4 coverage textures) in list
  order — invert, opacity, mode table of `MaskMode` — and writes layer × matte; longer lists
  chain through an intermediate matte. The CPU fallback clips with the combined
  `QPainterPath`s (hard edges; feather, expansion and partial opacity ignored).
* Masks are evaluated at the requested frame exactly (`AnimatedPath::evaluate`), so viewer,
  playback and export agree.

### Adjustment clips, generators, captions

* **Adjustment clips** (`ClipKind::Adjustment`): `compositeTimeline()` notices a track frame
  that shows one, moves the accumulator aside as the "below" texture, copies it into a fresh
  accumulator and draws the clip with that texture as its source (`FrameContext::below`,
  output-sized, so layers never go above output resolution). A single adjustment clip is drawn
  in two passes: `ClipBlend::Erase` (the footprint - crop, masks, transform - times opacity is
  removed: `GL_ZERO, GL_ONE_MINUS_SRC_ALPHA`) and `ClipBlend::Add` (the processed picture,
  `GL_ONE, GL_ONE`), so inside the footprint the result replaces the tracks below and outside
  it they are untouched. Transition frames use the transition shader over the copy. Cost: one
  full-frame copy + one footprint pass per adjustment track; nothing without one. Fast path
  (`hasIdentityFootprint()`: no transform / crop / mask, opacity 100 %, the usual "grade or
  blur everything"): no copy and no footprint pass - the layer path reads the accumulator and
  `ClipBlend::Replace` writes the result back; with no active effect the clip is skipped.
  1080p under llvmpipe (`test_adjustment cost`): plain scene 36 ms, + identity adjustment clip
  27 ms (skipped), + adjustment with saturation 71 ms (two full-frame passes). Works per
  nesting level. CPU fallback: the same with QImage copies, `DestinationOut` + `Plus`.
* **Generators** (`ClipKind::Generator`, `render/Generators.*`, `render/GlGenerators.*`):
  `resolveGenerator()` turns the clip's parameters at a frame into a `GeneratorFrame`; the GPU
  draws its pattern with one shader (`uMode`: four-colour / linear / radial gradient, SMPTE or
  full-field bars, grey scale, noise from an integer hash, countdown dial) into a per-frame
  target at output size (x zoom up to 2), and text (countdown number, timecode) through the
  title texture cache; the CPU fallback evaluates the same formulas per pixel
  (`renderGeneratorImage()`).
* **Captions**: after the video tracks of the root timeline, `drawSubtitles()` draws the
  caption of every enabled subtitle track (`RenderRequest::subtitles`, on for the viewer; export
  sets it only for "burn into video"), rasterised by TitleRenderer from
  `resolveCaptionStyle()` (caption → track → default style) and placed by `captionOffset()`
  (bottom / top / middle, alignment, margins in percent of the frame).

### Retimed clips (speed changes, spec section 31)

The source time of every frame comes from `core/Retime.h` (`clampedSourceTimeUs()`), so
viewer, playback, export and the CPU fallback show the same frame. With Frame Blend or Optical
Flow (`retime::effectiveProcess()`), a clip whose source time falls between two source frames
decodes the next frame too, on a second decoder of the pool positioned one frame later (both
keep decoding forward), converts both frames to RGB at the size the clip covers and builds the
in-between in `GlRetime`: a one-pass blend, or the optical flow (luma pyramid with level 0 at
most 640 px wide, per level a 5x5 search around the doubled coarser motion with a 3x3 SAD on
the symmetric positions A(p - w v) / B(p + (1 - w) v), cost-weighted smoothing, sub-pixel
parabola on level 0, then a warp that falls back to the blend where the two samples disagree).
Costs (1080p): blend ~0.5 ms; flow ~2-4 ms on a GTX 1650 class GPU, ~0.3-0.6 s on llvmpipe
(export fine, preview slow: use Nearest / Frame Blend). Nested clips use Nearest. Tests:
`test_retimerender`.

### Nested timelines (compound clips)

A `ClipKind::Nested` clip shows another timeline, found in the root snapshot's
`TimelineData::nested` map (filled by `Timeline::snapshot()` / `Project::renderSnapshot()`).
The decode stage walks into visible nested timelines, so their media joins the frame's parallel
decode and the decode-ahead (`DecodedClip` is keyed by clip and source time: the same nested
clip can be visible at two times through two instances). Compositing: `compositeTimeline()`
renders the nested timeline at the parent's frame mapped with `nestedFrameAt()` into a pooled
RGBA16F target sized like the area it covers in the output (times its zoom, at most 2x),
cached for the rest of the frame (`m_nested`, repeated instances at the same frame), then
the clip draws it as a texture source, so its transform, crop, opacity, effects and
transitions apply to the nested picture as a whole. The CPU fallback renders nested timelines
recursively into images; the AudioMixer mixes them through a child mixer per Nested clip
(clipping only on the master). Depth is limited to 16 (recursion is prevented in core; this
only guards damaged files). Cost: one extra full-size pass per visible nested level, no extra
decoding; `tests/render/test_nested.cpp` checks nested against flattened output.

### Cancellation and statistics

* `RenderRequest::cancelled` (optional `shared_ptr<const atomic<bool>>`): a request whose flag
  is set before the render thread picks it up completes at once with `ok = false,
  cancelled = true`. Playback cancels its stale look-ahead on a seek.
* `RenderedFrame` reports `renderMs` and its stages (render-thread wall clock): `decodeMs`
  (incl. hardware download; ~0 when the decode-ahead finished in time), `uploadMs` (plane
  textures), `compositeMs` (drawing), `outputMs` (output slot, fence/flush, readback). On
  llvmpipe the GPU work itself shows up in `outputMs` (it runs at the flush).

## Threading and GL context model

* `Compositor` (constructed on the GUI thread: it creates the `QOffscreenSurface`) starts one
  `QThread`. That thread creates a `QOpenGLContext` (3.3 core) **sharing with
  `QOpenGLContext::globalShareContext()`** (`Qt::AA_ShareOpenGLContexts`, set in `main.cpp`),
  makes it current once and keeps it current for its lifetime. All GL objects, the decoder
  pool and the title cache belong to that thread; nothing else touches them.
* Requests are queued from any thread and served in order; results come back through
  `std::future`. Output textures are recycled only after the last `RenderedFramePtr`
  referencing them is released (the deleter only flips a flag under a mutex).
* Each output texture carries a `GLsync` fence created after its last command and flushed, so
  a viewer context can `glWaitSync` on it (cross-context synchronisation).
* Decoding of the clips visible in one frame runs in parallel on a 4-thread pool owned by the
  renderer; each decoder is used by one thread at a time.
* If context creation, `makeCurrent`, the version check (≥ 3.3, not ES) or shader compilation
  fails, the thread uses `CpuRenderer` instead (QPainter; transform, opacity, crop, titles,
  solids; every transition as a dissolve; effects skipped with one warning per effect id).
* Decoders unused for 10 s are closed; render targets unused for 120 frames are deleted.
* A decoder with a decode-ahead in flight (`DecoderPool::Entry::ahead`) is only used or closed
  after `waitAhead()`.

## Files

| File | What |
|---|---|
| `Compositor.*` | public facade, request queue, render thread, backend selection |
| `RenderBackend.h` | internal backend interface (GPU / CPU) |
| `GlRenderer.*` | GPU backend: track walk, decode stage, clip drawing (direct and layer paths), title cache |
| `GlKit.*` | GL toolkit: common vertex shader, `GlProgram`, pooled `RenderTarget`s, blit/fill, separable Gaussian blur (downscaled for large sigma) |
| `GlVideo.*` | persistent plane textures (R8/RG8/R16/RG16/RGBA8) + the YUV → RGB shader (crop, rotation, edge antialiasing) |
| `GlEffects.*` | the eight built-in effects and **their registration table** |
| `GlMasks.*` | clip mask pass (analytic shapes, path coverage, feather, combine) |
| `GlTransitions.*` | the four built-in transitions and **their registration table** |
| `GlOutput.*` | output slots (shared textures + fences), PBO readback |
| `CpuRenderer.cpp` | QPainter fallback |
| `TimelineEval.*` | pure: transition spans, track evaluation, source time clamping, clip transform |
| `TitleRenderer.*` | `TitleStyle` (cache key) + QPainter title rasteriser |
| `CaptionRenderer.*` | caption style resolution and placement (subtitle tracks) |
| `Generators.*` | generator parameter resolution, CPU raster, shared bar table / noise hash |
| `GlGenerators.*` | the generator pattern shader |
| `YuvMath.*` | YUV → RGB coefficients (unit-tested on the CPU) |
| `DecoderPool.*` | `VideoDecoder` reuse by path and position |
| `AudioMixer.*` | audio mixing |
| `GlRetime.*` | in-between frames of retimed clips: frame blend, optical flow (coarse-to-fine symmetric block matching) |
| `TimeStretch.*` | WSOLA time stretch (pitch-corrected retimed audio) + `SourceCache` |
| `RenderFrameCommand.*` | `--render-frame` |

## Performance (1080p25, `build/test_renderperf`)

GTX 1650 (NVIDIA 580, CUDA decode), ms per frame:

| Timeline | viewer (texture) | viewer Half | export, 3 in flight | export, 1 at a time |
|---|---|---|---|---|
| 1 video layer | 2.7 | 2.9 | 4.1 | 4.9 |
| 2 layers + blur (r = 12 px) | 4.4 | 3.2 | 4.9 | 6.2 |
| 3 layers + title | 3.7 | 2.9 | 4.6 | 5.5 |
| CPU fallback, 1 layer | — | — | — | 7.0 |

Under xvfb (llvmpipe = software OpenGL, what ctest measures, 12 frames per case): 25 / 171 /
162 ms (texture) and 8 / 32 / 35 ms at Half. On the GPU decoding dominates; a static title costs
~0.3 ms/frame including readback.
The per-clip decode-ahead (above) hides the decode time on the render thread: with software
decoding and llvmpipe a 1080p60 screen recording went from 46 to 60 fps (see
`src/ui/viewer/readme.md`).

## How to extend

### A new video effect
1. **Descriptor**: append `descriptor("my_effect", ...)` with its parameters to
   `EffectRegistry::registerBuiltins()` in `src/core/BuiltinEffects.cpp` (the Inspector builds
   its UI from it; the id is stored in project files).
2. **Shader + class**: in `GlEffects.cpp` derive from `PassEffect` (one pass over the layer,
   uniforms set between `begin()` and `draw()`), or from `GlEffect` for multi-pass effects
   (use `GlKit::gaussianBlur()`, `kit.acquire()` for temporaries). Read parameters with
   `in.params->value(key, in.localFrame, default)`; multiply pixel sizes by `in.scale`.
   Override `paddingPx()` if the effect draws outside the clip, `isIdentity()` to skip no-ops.
3. **Registration**: add `{"my_effect", &make<MyEffect>}` to `kEffectTable` (one line).
4. **Test**: a golden-pixel case in `tests/render/test_compositor.cpp` (`colourEffects` /
   `geometricEffects` show the pattern). The CPU fallback skips unknown effects automatically.

### A new transition
1. Descriptor (category `Transition`) in `BuiltinEffects.cpp`.
2. In `GlTransitions.cpp`: a fragment shader using `uA`, `uB`, `uProgress` (+ own uniforms set
   in a `SetExtra` function), a `makeX()` factory and a row in `kTransitionTable`. A and B are
   already transformed, premultiplied, output-sized; a missing side is transparent.
3. Test at a known progress (see `otherTransitions`). Timing semantics need no change: they
   live in `TimelineEval` and are shared with the audio crossfade.

### A new generator
1. Id + parameter keys in `core/Generators.h`, descriptor (category Generator, storage
   "title") in `registerGenerators()` (`core/BuiltinEffects.cpp`).
2. A `GeneratorFrame::Kind`, its resolution in `resolveGenerator()`, the per-pixel formula in
   `patternAt()` (CPU) and the same in the `GlGenerators` shader (new `uMode`), or only a
   `textStyle` for text generators.
3. A case in `tests/render/test_generators.cpp` (determinism + GPU/CPU agreement).

### A new clip kind
Add the `ClipKind`, then a case in `GlRenderer::resolveSource()` producing a texture
(`ClipSource::Kind::Texture`, cache it like titles) or a colour, and the same in
`CpuRenderer::drawClip()`.

### A new pixel format
Add it in `media::PixelFormat` (see `src/media/readme.md`), map plane formats in
`planeFormat()`/`layoutOf()` in `GlVideo.cpp`, and its bit depth / sample scale in `YuvMath.cpp`.

## Known limitations / TODO

* TODO(render): no mipmaps/area filter when a source is minified a lot (e.g. 4K at Quarter
  preview) — bilinear sampling can alias; render the YUV pass at an intermediate size first.
* TODO(render): HDR (PQ/HLG) frames are composited as SDR (no tone mapping), chroma siting is
  assumed centred, BT.2020 is converted with its matrix but not gamut-mapped to BT.709.
* TODO(render): zero-copy hardware frames (VAAPI dma-buf → EGLImage when the compositor's
  context is EGL, CUDA → GL interop) — the GPU path still downloads hardware frames (VAAPI:
  streaming-load copy, see src/media) and uploads planes. Needs testing on Intel/AMD hardware
  with a session (the build machine's render nodes are not accessible to the agents).
* The CPU fallback renders every transition as a dissolve and skips effects.
* Retimed audio: time stretched (WSOLA, `TimeStretch`) with pitch correction, else resampled
  along the retime curve; freeze / reverse parts are muted.
* An output slot released while a viewer's draw that samples it is still in flight could in
  theory be overwritten early; keep the frame until the next one is displayed (documented above).
