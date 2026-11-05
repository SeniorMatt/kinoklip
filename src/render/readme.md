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
| `AudioNormalize.h` | Clip > Normalize Audio Levels (spec 37.35): `audionorm::measure()` decodes a clip's source range in 100 ms blocks into a `LoudnessMeter` (true peak, integrated LUFS) plus a sample peak, cancellable, with progress; `gains()` (relative / independent) |
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
kk::MixLevels levels; mixer.setLevels(&levels);  // optional: per track / bus / output peaks
```

The mixer graph (Audio page, spec 37; `AudioMixerGraph.cpp`): each audio track's clips are
summed into a track buffer (clip volume, clip pan, fades, crossfades: `AudioMixer.cpp`), then the
track's channel strip runs (`ChannelProcessor`): **channel format** (mono = (L + R) / 2) ->
**EQ** (`Equalizer`, 6 RBJ biquads, `Biquad`) -> **compressor** -> **limiter** (`Dynamics`) ->
**pan** (constant power, -3 dB law: centre is 3.01 dB below a hard pan, normalised to unity at
centre so an untouched track is unchanged; stereo tracks pan as a balance) -> **fader** (track
automation or the static value) -> **mute / solo**; the result is added to every bus the track is
routed to. Each bus: sum -> channel format -> EQ -> compressor -> limiter -> fader -> mute. The
output bus (`MixerData::outputBus`) goes to the soft clipper and out, so playback and export
render the same bus. Gains glide over ~5 ms and EQ parameters over ~10 ms (no clicks, no zipper
noise); a mix call that does not continue the previous one clears every filter and envelope
first, so a range mixes to the same samples in one call, in playback chunks and after a seek
(`test_mixergraph`). Channels with no sound and no tail skip their processing. Metering
(`MixLevels`): post-fader peaks per track and bus and of the output; `LoudnessMeter` (BS.1770-4:
K-weighting, momentary / short-term / gated integrated LUFS, 4x true peak) is run by the playback
feeder on the output.

The render cache stores pictures only (video chunks, `RenderCacheKeys`): audio is always mixed
live from the snapshot, so a mixer change can never hear stale cached audio. Sound-only edits
(mixer, automation, clip volume) leave the pictures alone: `render::samePictures()` lets the
playback engine keep the rendered frames (`PlaybackEngine::onTimelineChanged()`).

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
                       → clip grade (GlGrade, Color page; a graded clip always takes this path)
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

### Video effects (v4 batch 1: keyers, colour, blur, light, stylize, distort)

Effects run on the clip's upright layer (premultiplied RGBA16F) in the Inspector's order, top
to bottom; the masks follow (below), so a mask is a garbage matte for a keyer. Files: the v0.1
effects in `GlEffects.cpp`, the v4 ones in `GlKeyers.cpp` (Chroma / Luma / HSL Key),
`GlColorEffects.cpp`, `GlBlurEffects.cpp`, `GlStylizeEffects.cpp` (Glow, Film Grain,
Chromatic Aberration, Edge Detect, Emboss, Stroke) and `GlDistortEffects.cpp` (Lens
Distortion, Ripple, Wave, Feathered Crop); `GlEffectBase.h` holds `PassEffect`, the GLSL
helpers (`kGlslCommon`: luma, straight colour, the keyers' view output) and the layer geometry
(`EffectInput::paddingPx` = transparent margin, `EffectInput::fps` for animated waves).
* **Keyers** compute a matte m from the straight colour and output premultiplied
  (colour · a · m, a · m), so the keyed clip goes through the normal premultiplied "over"
  everywhere (viewer and render). Chroma key distance: luma-normalised BT.709 CbCr (see
  `fxmath::chromaDistance()`). Shrink / grow and edge blur run on a separate matte target
  (`GlKit::morphology()`, `GlKit::gaussianBlur()`), never on the colour; without them the key
  is one pass.
* **CPU reference** `EffectMath.*` (`fxmath::`): the per-pixel formulas of the keyers and the
  colour effects and their parameter resolution (`*Params`), shared by the GPU passes'
  uniforms, the tests (GPU vs CPU within 3 / 255) and the CPU fallback (`applyPointEffect()`).
* **Second batch** (`GlGradeEffects.cpp` Curves / HSL Adjust / Apply LUT / Vibrance /
  Channel Mixer / Duotone, `GlFocusEffects.cpp` Unsharp Mask / Tilt-Shift / Lens Blur / Noise
  Reduction, `GlLightEffects.cpp` Light Rays / Bloom / Halation / Lens Flare,
  `GlLookEffects.cpp` Pixelate / Halftone / Scanlines / Pencil Sketch / Abstraction / Prism,
  `GlWarpEffects.cpp` Twirl / Bulge / Kaleidoscope / Mirror Split / Corner Pin / Camera Shake /
  Tile, `GlTimeEffects.cpp` Echo / Strobe / Flicker). Pointwise ones use `fx::ColorEffect`
  (GlEffectBase.h: GLSL `apply()` + up to 6 uniforms) and the formulas of `EffectMathGrade.cpp`;
  Curves upload a 256 x 1 composite table, Apply LUT a 3D (or N x 1) float texture of the
  `CubeLut` (re-uploaded only when the cached LUT object changes).
* **Time effects** (Echo, Strobe) read the clip at other frames through
  `EffectInput::history` (`LayerHistory::layerAt()`): `GlRenderer::queueDecode()` also decodes
  the frames `timefx::clipFrames()` lists, and `renderLayerAt()` draws the clip's source at
  such a frame into a layer of the same geometry and applies the effects above the asking
  one. Deterministic (no state carried between frames), so scrubbing and the render agree.
  Clips without time effects take the unchanged path. The CPU fallback skips them.
* **Margins that depend on the clip size**: `GlEffect::contentPaddingPx()` (default
  `paddingPx()`) gets the content size; the radial and zoom blurs use it when Repeat Edge
  Pixels is off.
* **Cost**: every v4 effect is bounded per pixel: colour / keys / distort one pass; box blur
  separable with downscaling; path blurs (directional, radial, zoom) 24 samples, two passes
  of 24 when the samples would be more than 1.5 px apart; morphology `2 x radius` fetches per
  axis in passes of at most 16 taps.

### Grades (`GlGrade`, `GradeEval`, Color page)

`ClipData::grade` is applied to the clip's layer after its effects and before the masks;
`TimelineData::grade` over the accumulator after every track (before captions). `GlGrade`
runs one pass per correcting node (plus a key pass for keyed nodes under a Layer mixer, a
pass per mixer); curve tables, LUTs and polygon points are float textures read with
`texelFetch`, so the CPU twin `GradeEval` (per pixel, `core/GradeMath`) gives the same pixels
within 2 levels. `RenderRequest::bypassGrades` (viewer "bypass all") skips both and the render
cache. Design: docs/color-page.md.

Dynamic Zoom (spec 24.20, core/DynamicZoom.h) is part of `clipTransform()`: the clip's framing
rectangle at the frame is scaled onto the whole frame before the Transform, so both renderers
apply it the same way; `maxZoom()` and `hasIdentityFootprint()` count it.
`RenderRequest::dynamicZoom = false` (the viewer's on-screen Dynamic Zoom controls) leaves it
out and skips the render cache.

### Scopes (`Scopes`)

Waveform / parade / vectorscope / histogram data from a small QImage of the program frame
(widgets in src/ui/scopes).

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

### Compositions (Compositing page, spec section 42a)

A clip with a `ClipData::composition` (core/Composition.h, node types in core/NodeRegistry.h)
shows the composition's output instead of its own picture; its crop, effects, masks,
transform, opacity, transitions and compositing then apply on top, so the Edit page viewer,
playback, the Cut page and the export (one `GlRenderer::resolveSource()`) all show it. A
composition whose MediaOut reads (through disabled nodes) an enabled MediaIn of the clip
itself (`compops::isPassThrough()`, e.g. the default MediaIn1 -> MediaOut1) is skipped: it
costs nothing and renders bit-identically.
* **Evaluation** (`GlComposition`): `compops::evaluationOrder()` lists the nodes the pulled
  node (MediaOut, or any node for the page's viewers) depends on; each runs once into a
  pooled RGBA16F target of the comp size (the frame at the resolution it covers in the
  output, x zoom up to 2, like generators: 1080p at Full). Targets are reference counted and
  go back to the pool after their last reader, so a chain of ten nodes keeps two or three
  alive. Disabled nodes, MediaOut and identity nodes (an effect's `isIdentity()`, a
  Transform at rest, a neutral Color Corrector, a Merge without foreground) alias their main
  input: no pass, no copy. Re-entrant per depth (a MediaIn of a nested clip may evaluate
  other compositions).
* **Passes**: MediaIn = the clip's own source (`resolveSource(..., false)`) fitted and centred
  like a clip at zoom 1, at the comp frame (timeline frame = clip start + comp frame, so the
  clip's speed and retime curve apply), or another media pool item (`CompNode::mediaId`)
  from its start through a per-frame stand-in clip in the decode stage (parallel decode and
  decode-ahead like any clip). Merge = one shader: foreground through the inverse of its
  centre / size / angle (edge antialiasing like the clip quads), blend mode on straight
  colours with the separable compositing formula (`compmath::blendChannel()`), opacity,
  then the mask. Dissolve and every mask input: `mix(input, result, mask alpha)` (one pass;
  "the effect applies only inside the mask"). Transform, Crop, Resize: one textured quad.
  Letterbox: copy + two bar fills. Background: the generator pattern pass (`GlGenerators`).
  Text: the title texture cache (TitleRenderer). Fast Noise: fractal value noise with an
  integer hash. Masks: `GlMasks` on a white layer (the clip-mask maths: analytic rectangle /
  ellipse, polygon coverage, feathered by the soft edge), united with a mask input by
  "over". Color Corrector / Channel Booleans: one pointwise pass each. Effect nodes
  ("fx:<id>") call the Edit page `GlEffect` with the node's parameters (pixel sizes in
  timeline px, `EffectInput::scale` = comp scale). Units and coordinates: core/NodeRegistry.h.
* **Viewer API**: `Compositor::renderCompositionNode(snapshot, clipId, nodeId, compFrame,
  size, quality, outputs, media, proxies)` = a `RenderRequest` with `compositionNode` set
  (MediaIn decodes through `media` / `proxies` like a timeline render; the page's viewers call
  `PlaybackEngine::renderCompositionNode()`, which passes the engine's own): the image of one
  node (0 = MediaOut) as the usual zero-copy texture (or image), never through the render
  cache. Cost per call: the MediaIn decodes (decode-ahead when the frame advances) plus the
  passes of the nodes the shown node depends on; no readback for `OutputTexture`. Snapshot:
  `Project::renderSnapshot(timelineUid)` (or `Timeline::snapshot()` for the active one).
* **Cache and change detection**: the render cache key hashes the composition without node
  positions, names, notes and view state (`compositionToJson(..., true)`) plus the media
  keys of other MediaIn items; a non-trivial composition marks the chunk like an effect
  (Smart mode caches it). `samePictures()` compares the whole clip, so playback re-renders
  after every graph change. Timeline thumbnails show source frames (as with effects).
* **CPU fallback** (`CpuComposition`): every built-in node with the same geometry and colour
  formulas (`CompMath`); masks have hard edges (soft edge ignored, as for clip masks);
  effect nodes: the pointwise ones (`fxmath::applyPointEffect()`), the others pass their
  input through (one warning per type).
* **Gaps**: time effects (Echo, Strobe) as nodes show the current frame only (no layer
  history inside a composition); Merge has no per-pixel "edges" options; nodes always work
  at comp resolution (no domain of definition yet: a small Text costs a full-frame pass);
  the clip's Edit page crop is in timeline pixels for a clip with a composition.
* **Cost** (1080p, `test_comprender performance1080p`, ten nodes: two merges with Background
  and Text, masked Gaussian Blur, Color Corrector, Transform, Fast Noise screened over):
  about two full-frame RGBA16F passes per node. llvmpipe (software GL, measured while the
  machine was heavily loaded, load average about 26): 685 - 825 ms per frame at Full and 190 - 260 ms at
  Half, against 46 - 61 ms for the same clip without a composition; single nodes cost
  50 - 100 ms there (blur 200 - 300 ms). On a GPU a 1080p pass costs a fraction of a millisecond,
  so the ten-node graph fits a 25 fps frame budget. Not measured on hardware here.

### Retimed clips (speed changes, spec section 29)

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

### Multicam clips

A Nested clip with `ClipData::multicamAngle >= 0` is resolved with
`NestedTimelines::findClip()` to the angle view of its multicam timeline (one video and one
audio track), so the nested pass composites, decodes and mixes only the active angle. The
multicam viewer's grid is an ordinary timeline of such clips scaled into cells, i.e. N nested
composites at cell size (decode stays full size: proxies will cut that cost). Test:
`test_multicamrender`.

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

### Proxies and the render cache

* `RenderRequest::proxies` (`ProxyMap`: media id -> `ProxyRef {path, displaySize}`): the
  decode stage opens the proxy instead of the original; `resolveSource()` (and the CPU path)
  keep the original's display size as `sourcePx`, so crop units and fitting do not change.
  nullptr (export) = originals. Built by `ProxySelection.h` (`makeProxyMap()`: Proxy Handling,
  optimized media, stale / missing links never used; `makeAudioPathMap()`: offline originals
  play their proxy's sound; `usesSubstitutes()`: viewer indicator).
* `RenderRequest::renderCache` (`RenderCacheView`): `Compositor::renderAsync()` replaces a
  request for a frame inside a cached chunk by a request for the chunk's one-clip timeline
  (`RenderedFrame::fromCache`), but only when `view->snapshot == request.timeline`.
  `RenderCacheKeys.h`: chunk grid, keys, Smart / User selection, `makeRenderCacheView()`.
  Bump `kRenderCacheVersion` whenever rendering changes for the same input.

### Decoder look-ahead

`RenderRequest::prepare` (`PrepareRange`) renders nothing: `Compositor` walks the range with
`DecodeTargets` and calls `DecoderPool::prepare()` for what comes into view, which opens, seeks
and decodes on the backend's worker threads. See `src/playback/readme.md` (Cuts) and the class
comment of `DecoderPool`. `Compositor::prepareStats()` has the counters.

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
* Decoding of the clips visible in one frame runs in parallel on a thread pool (4 + 2 for the
  decoder look-ahead) owned by the
  renderer; each decoder is used by one thread at a time.
* If context creation, `makeCurrent`, the version check (≥ 3.3, not ES) or shader compilation
  fails, the thread uses `CpuRenderer` instead (QPainter; transform, opacity, crop, titles,
  solids; every transition as a dissolve; the pointwise effects (keyers, colour) through
  `fxmath::applyPointEffect()`, the other effects skipped with one warning per effect id).
* Decoders unused for 10 s are closed; render targets unused for 120 frames are deleted.
* A decoder with a decode-ahead in flight (`DecoderPool::Entry::ahead`) is only used or closed
  after `waitAhead()`.

## Files

| File | What |
|---|---|
| `Compositor.*` | public facade, request queue, render thread, backend selection |
| `RenderBackend.h` | internal backend interface (GPU / CPU) |
| `DecodeTargets.*` | the media positions a timeline frame needs, without any decoder (decoder look-ahead) |
| `GlRenderer.*` | GPU backend: track walk, decode stage, clip drawing (direct and layer paths), title cache |
| `GlInfo.*` | the OpenGL vendor / renderer / version strings of the context `GlRenderer` made (`knownGlDriverInfo()`), for Help > Report a Bug; never creates a context |
| `GlKit.*` | GL toolkit: common vertex shader, `GlProgram`, pooled `RenderTarget`s, blit/fill, separable Gaussian and box blur (downscaled for large radii), separable erode / dilate (`morphology()`) |
| `GlVideo.*` | persistent plane textures (R8/RG8/R16/RG16/RGBA8) + the YUV → RGB shader (crop, rotation, edge antialiasing) |
| `GlEffects.*` | the eight v0.1 effects and **the registration point** `builtinGlEffects()` (joins the tables of the files below) |
| `GlEffectBase.h` | internal: `PassEffect`, GLSL helpers, layer geometry for the effect files |
| `GlKeyers.cpp` / `GlColorEffects.cpp` / `GlBlurEffects.cpp` / `GlStylizeEffects.cpp` / `GlDistortEffects.cpp` | the v4 effects by library folder, each with its factory table |
| `GlGradeEffects.cpp` / `GlFocusEffects.cpp` / `GlLightEffects.cpp` / `GlLookEffects.cpp` / `GlWarpEffects.cpp` / `GlTimeEffects.cpp` | the second batch: colour (curves, LUT, ...), blur and sharpen, light, stylize, distort and tile, time |
| `EffectMath.*`, `EffectMathGrade.cpp` | CPU reference of the pointwise effects, parameter resolution shared with the GPU, CPU fallback |
| `CubeLut.*` | .cube 1D / 3D LUT parser, interpolation, thread-safe file cache (via `media::MediaSource`) |
| `TimeEffects.*` | pure: the clip-local frames Echo and Strobe read (decode planning and the effects share it) |
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
| `AudioMixer.*` | audio mixing: clips, fades, transitions (`AudioMixer.cpp`) and the mixer graph of tracks and buses (`AudioMixerGraph.cpp`, state in `MixerChannel.h`) |
| `ChannelProcessor.*` | one mixer channel: mono fold, EQ, dynamics, smoothed pan / fader gains, peaks |
| `Biquad.*` | RBJ cookbook biquad design (high / low pass, shelves, bell, notch) and response |
| `Equalizer.*` | smoothed 6-band EQ (real-time safe) |
| `Dynamics.*` | compressor (dB-domain attack / release, soft knee, make-up) and peak limiter |
| `LoudnessMeter.*` | BS.1770-4 / EBU R128 loudness and 4x true peak |
| `MixLevels.*` | meter readings of a mix call (`MixLevels`) and the time-stamped `MeterFrame` |
| `GlComposition.*` | Compositing page: GPU evaluation of a node graph (passes, per-evaluation target reference counts) |
| `CpuComposition.*` | Compositing page: CPU fallback evaluation |
| `CompMath.*` | Compositing page: node geometry, Background / Text / noise resolution and colour formulas shared by GPU and CPU |
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

### A new mixer processor (e.g. a gate)
Write the DSP as a class like `Dynamics` (real-time safe, `setTarget()` / `reset()` /
`process()`, its own smoothing), add its settings to `ChannelStrip` (core/MixerData.h, JSON in
core/MixerJson.cpp), run it from `ChannelProcessor::processInserts()` at its place in the signal
flow, and test it with synthesized signals (tests/render/test_mixerdsp.cpp).

### A new video effect
1. **Descriptor**: append `effect("my_effect", ..., effectgroups::Blur, ...)` with its
   parameters to `EffectRegistry::registerVideoEffects()` in `src/core/BuiltinVideoEffects.cpp`
   (ids and keys in `core/VideoEffectKeys.h`; the Inspector builds its UI from it, the library
   puts it in its folder; the id is stored in project files).
2. **Shader + class**: in the file of its folder derive from `PassEffect` (one pass over the layer,
   uniforms set between `begin()` and `draw()`), or from `GlEffect` for multi-pass effects
   (use `GlKit::gaussianBlur()`, `kit.acquire()` for temporaries). Read parameters with
   `in.params->value(key, in.localFrame, default)`; multiply pixel sizes by `in.scale`.
   Override `paddingPx()` if the effect draws outside the clip, `isIdentity()` to skip no-ops.
3. **Registration**: add `{fxid::MyEffect, &make<MyEffect>}` to that file's table (one line).
4. **Pointwise effect?** Put the formula and its `*Params` into `EffectMath`, upload the same
   values as uniforms, and list it in `fxmath::isPointEffect()` / `applyPointEffect()`: the CPU
   fallback then applies it too.
5. **Test**: in `tests/render/test_videoeffects.cpp` add a row to `pointEffectsMatchReference`
   (pointwise: GPU and CPU fallback against `EffectMath`) or an analytic case like
   `blursSmooth` / `distortAndCrop`, and the id to `kNewEffects` (defaults, round trip). The
   CPU fallback skips other effects automatically.

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
* The CPU fallback renders every transition as a dissolve and skips the non-pointwise effects
  (blurs, light, stylize, distort, the keyers' shrink / grow and edge blur).
* Retimed audio: time stretched (WSOLA, `TimeStretch`) with pitch correction, else resampled
  along the retime curve; freeze / reverse parts are muted.
* An output slot released while a viewer's draw that samples it is still in flight could in
  theory be overwritten early; keep the frame until the next one is displayed (documented above).

### Fades and Clip Attributes (spec section 31)
* `clipOpacity()` (TimelineEval) = keyframed opacity x `videoFadeFactor()` of the clip's fade
  handles; GPU, CPU and the adjustment-clip identity test use it.
* `GlVideo::lookFor(frame, ClipData::attributes)` gives the rotation (file + attribute), the
  data levels used for YUV -> RGB and the deinterlacing field; the YUV shader's
  `fieldSample()` samples only the kept field's rows (two fetches 2 rows apart). The display
  size follows the pixel aspect override and the total rotation (`resolveSource()`); proxies
  keep the original's size, turned by the extra rotation. CPU fallback: pixel aspect and
  rotation only.
* AudioMixer: fade handles are equal-power gain ramps in the gain envelope; channel routing is
  applied by `media::AudioDecoder::setChannelMapping()` (a swresample matrix; a change
  re-decodes from the exact sample and clears the retime source cache).
