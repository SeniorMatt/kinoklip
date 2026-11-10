// SPDX-License-Identifier: GPL-3.0-or-later
// EffectMathTemporal — the arithmetic of the multi-frame effects (spec 60) in plain C++: the
// weights of Motion Trails and Smear (shared with the GPU effects) and the per-pixel and
// per-frame rules of Deflicker, Color Stabilizer and Automatic Dirt Removal / Dust Buster (the
// reference the GPU shaders are tested against; the GPU computes the same with textures). Pure
// functions on straight or premultiplied values as the caller gives them.
#pragma once

#include <array>
#include <vector>

namespace kk::render::fxmath {

/// One colour value with alpha (any consistent encoding).
struct Rgba
{
    float r = 0.f, g = 0.f, b = 0.f, a = 1.f;
};

/// Weights of the layer itself (first) and its trails, nearest first, for Motion Trails: trail i
/// (1 = the previous frame) weighs opacity * keep^i with keep = 1 - fade. With @p normalise
/// (Average mode) the weights are divided by their sum, so a still picture keeps its value;
/// without it (Lighten mode) the layer weighs 1 and a trail frame counts as its own colour mixed
/// toward the layer by its weight, the newest light winning (a per-channel maximum).
[[nodiscard]] std::vector<double> trailWeights(
    int trails, double fade, double opacity, bool normalise);

/// Weights of the layer itself (first) and the frames at the given distances (in frames) for
/// Smear: a frame @p distance away weighs (1 - falloff)^distance, the weights are divided by
/// their sum and mixed with the plain layer by @p amount (the layer's weight is 1 - amount plus
/// its share).
[[nodiscard]] std::vector<double> smearWeights(
    const std::vector<int>& distances, double falloff, double amount);

/// The temporal consensus of the neighbours of a pixel (two or four, in any order): per channel
/// the mean of the middle two of four sorted values (two neighbours count twice). @p spread gets
/// the largest distance between the middle two over the channels.
[[nodiscard]] Rgba temporalMedian(const std::vector<Rgba>& neighbours, float* spread);

/// True if @p cur is dirt: it differs from the consensus by more than @p contrast in some
/// channel while the neighbours agree within @p motion (all in 0..1 of full scale).
[[nodiscard]] bool isDirt(
    const Rgba& cur, const std::vector<Rgba>& neighbours, float contrast, float motion);

/// Deflicker: the gain for the current frame (means[0]) toward the average of all the means
/// (the window including the frame itself). @p perChannel gives one gain per channel, otherwise
/// the brightness (Rec. 709 luma) gain for all. @p limit is the extra gain allowed (0.5 = 2/3 to
/// 1.5), @p strength 0..1 the share of the correction applied.
[[nodiscard]] std::array<float, 3> deflickerGain(
    const std::vector<Rgba>& means, bool perChannel, float strength, float limit);

/// Color Stabilizer matches: both channels' gains, only the brightness, or only the colour
/// balance with the brightness of the current frame kept.
enum class StabilizerMatch {
    Both,
    Exposure,
    WhiteBalance
};

/// Color Stabilizer: the gain that brings the mean colour @p current to @p reference (straight
/// colours), limited and mixed like deflickerGain().
[[nodiscard]] std::array<float, 3> stabilizerGain(
    const Rgba& current, const Rgba& reference, StabilizerMatch match, float strength, float limit);

} // namespace kk::render::fxmath
