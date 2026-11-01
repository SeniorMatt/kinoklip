// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>

namespace kk {

// Exact frame rate / time base (e.g. 30000/1001).
struct Rational
{
    int num = 25;
    int den = 1;
    constexpr double toDouble() const { return den ? double(num) / double(den) : 0.0; }
    constexpr bool isValid() const { return num > 0 && den > 0; }
    friend constexpr bool operator==(const Rational&, const Rational&) = default;
};

// Timeline frame <-> microseconds helpers (see plan.md §5.1).
inline int64_t framesToUs(int64_t frames, Rational fps)
{
    return fps.isValid() ? (int64_t)std::llround(double(frames) * 1e6 * fps.den / fps.num) : 0;
}
inline int64_t usToFrames(int64_t us, Rational fps)
{ // floor
    return fps.isValid() ? (int64_t)std::floor(double(us) * fps.num / (1e6 * fps.den) + 1e-9) : 0;
}
inline int64_t framesToSamples48k(int64_t frames, Rational fps)
{
    return fps.isValid() ? (int64_t)std::llround(double(frames) * 48000.0 * fps.den / fps.num) : 0;
}

} // namespace kk
