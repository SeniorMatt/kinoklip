// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeGrid — grid step helpers shared by the curve views and the Keyframes panel ruler.
#pragma once

#include "core/Rational.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kk::keyframegrid {

/// "Nice" value step (1, 2, 5 x 10^n) giving about @p lines grid lines over @p span.
[[nodiscard]] inline double niceStep(double span, int lines)
{
    const double raw = std::max(1e-12, span / std::max(1, lines));
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        if (m * mag >= raw)
            return m * mag;
    }
    return 10.0 * mag;
}

/// Time grid step in frames so that ticks are at least @p minPx apart at @p pixelsPerFrame:
/// whole frames (1, 2, 5, 10) while zoomed in, then whole seconds and minutes of @p fps.
[[nodiscard]] inline int64_t timeStepFrames(Rational fps, double pixelsPerFrame, double minPx)
{
    const double framesPerSecond = fps.den > 0 && fps.num > 0 ? double(fps.num) / fps.den : 25.0;
    const int64_t fpsRounded = std::max<int64_t>(1, std::llround(framesPerSecond));
    const int64_t candidates[] = {1, 2, 5, 10, fpsRounded / 2, fpsRounded, 2 * fpsRounded,
        5 * fpsRounded, 10 * fpsRounded, 30 * fpsRounded, 60 * fpsRounded, 300 * fpsRounded,
        600 * fpsRounded, 1800 * fpsRounded, 3600 * fpsRounded};
    int64_t best = candidates[std::size(candidates) - 1];
    // Candidates are increasing except where fps / 2 < 10; take the smallest that fits.
    for (int64_t c : candidates) {
        if (c > 0 && c * pixelsPerFrame >= minPx && c < best)
            best = c;
    }
    return best;
}

} // namespace kk::keyframegrid
