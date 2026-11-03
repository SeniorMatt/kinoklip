// SPDX-License-Identifier: GPL-3.0-or-later
// TimeEffects — which clip-local frames the time effects (Echo, Strobe) read, shared by the
// GPU renderer's decode planning (GlRenderer::queueDecode()) and the effects themselves
// (GlTimeEffects.cpp), so both always agree. Pure functions.
#pragma once

#include "core/TimelineData.h"

#include <cstdint>
#include <vector>

namespace kk::render::timefx {

/// Most distinct frames one clip may read in one output frame (bounds the decode cost of
/// chained time effects).
inline constexpr int kMaxClipFrames = 9;

/// True for Echo and Strobe.
[[nodiscard]] bool isTimeEffect(const QString& type);
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
