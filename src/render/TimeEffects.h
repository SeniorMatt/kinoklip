// SPDX-License-Identifier: GPL-3.0-or-later
// TimeEffects — which clip-local frames the effects that read other frames of their clip
// (Echo, Strobe and, since spec 60, Motion Trails, Smear, Frame Replacer, Motion Blur,
// Deflicker, Color Stabilizer, Automatic Dirt Removal, Dust Buster) read, shared by the GPU
// renderer's decode planning (GlRenderer::queueDecode()) and the effects themselves
// (GlTimeEffects.cpp, GlTemporalEffects.cpp, GlRepairEffects.cpp), so both always agree.
// Pure functions.
//
// The mechanism: an effect that needs neighbouring frames is listed in framesRead(); the
// renderer decodes those frames of the same clip before drawing (at the clip's own frame steps,
// so trim, speed, reverse and freeze are respected: local frame f - 1 of a clip at speed 2 is
// two source frames back) and hands each one to the effect through EffectInput::history
// (LayerHistory::layerAt(), the clip's picture with the effects before it applied). Frames
// outside the clip's media repeat the first or last frame of the media (the decoder clamps the
// source time); frames of the clip's handles (media beyond the trim) are real frames.
#pragma once

#include "core/TimelineData.h"

#include <cstdint>
#include <vector>

namespace kk::render::timefx {

/// Most distinct frames one clip may read in one output frame (bounds the decode cost of
/// chained time effects). It equals the decoders the pool opens per file (DecoderPool::
/// kMaxPerPath): every frame of a clip is decoded by a decoder of its own, so more frames would
/// only be replaced by the nearest available one.
inline constexpr int kMaxClipFrames = 6;

/// True for the effects that read other frames of their clip: Echo, Strobe and the multi-frame
/// effects of spec 60 (framesRead()).
[[nodiscard]] bool isTimeEffect(const QString& type);
/// Appends to @p out the clip-local frames, other than @p localFrame, that the effect @p type
/// with @p params reads to draw @p localFrame, nearest first (the order effects use to find a
/// substitute when a frame is missing). Nothing for effects that read no other frame and for
/// parameters that make the effect do nothing. Frames before the clip start or after its end
/// are listed as they are (negative frames included): the renderer repeats the edge frame of
/// the media (and of a generated clip) for them.
void framesRead(
    const QString& type, const ParamSet& params, int64_t localFrame, std::vector<int64_t>* out);
/// Strobe: the held frame of @p localFrame (every Hold frames the picture is renewed).
[[nodiscard]] int64_t strobeFrame(const ParamSet& params, int64_t localFrame);
/// Echo: the earlier frames blended into @p localFrame, nearest first (frames before the
/// clip start are left out).
void echoFrames(const ParamSet& params, int64_t localFrame, std::vector<int64_t>* out);
/// Every clip-local frame @p clip's enabled time effects read to draw @p localFrame,
/// @p localFrame first, without duplicates, at most kMaxClipFrames (chained time effects
/// compose: an Echo after a Strobe reads the held frames of its echoes).
[[nodiscard]] std::vector<int64_t> clipFrames(const ClipData& clip, int64_t localFrame);

} // namespace kk::render::timefx
