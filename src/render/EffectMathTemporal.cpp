// SPDX-License-Identifier: GPL-3.0-or-later
// EffectMathTemporal implementation.
#include "render/EffectMathTemporal.h"

#include <algorithm>
#include <cmath>

namespace kk::render::fxmath {

namespace {

float luma(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

std::array<float, 3> limited(std::array<float, 3> gain, float strength, float limit)
{
    const float m = 1.f + limit;
    for (float& x : gain) {
        x = std::clamp(x, 1.f / m, m);
        x = 1.f + strength * (x - 1.f);
    }
    return gain;
}

} // namespace

std::vector<double> trailWeights(int trails, double fade, double opacity, bool normalise)
{
    const double keep = 1.0 - std::clamp(fade, 0.0, 1.0);
    std::vector<double> w {1.0};
    double total = 1.0, trail = opacity;
    for (int i = 0; i < trails; ++i) {
        trail *= keep;
        w.push_back(trail);
        total += trail;
    }
    if (normalise) {
        for (double& x : w)
            x /= total;
    }
    return w;
}

std::vector<double> smearWeights(const std::vector<int>& distances, double falloff, double amount)
{
    const double keep = 1.0 - std::clamp(falloff, 0.0, 1.0);
    std::vector<double> raw;
    double total = 1.0;
    for (const int d : distances) {
        raw.push_back(std::pow(keep, double(std::abs(d))));
        total += raw.back();
    }
    std::vector<double> w {1.0 - amount + amount / total};
    for (const double r : raw)
        w.push_back(amount * r / total);
    return w;
}

Rgba temporalMedian(const std::vector<Rgba>& neighbours, float* spread)
{
    std::vector<Rgba> n = neighbours;
    if (n.size() == 2) {
        n.push_back(n[0]);
        n.push_back(n[1]);
    }
    float widest = 0.f;
    float mid[4];
    for (int c = 0; c < 4; ++c) {
        float v[4];
        for (size_t i = 0; i < 4; ++i) {
            const Rgba& p = n[std::min(i, n.size() - 1)];
            v[i] = c == 0 ? p.r : c == 1 ? p.g : c == 2 ? p.b : p.a;
        }
        std::sort(v, v + 4);
        mid[c] = 0.5f * (v[1] + v[2]);
        widest = std::max(widest, v[2] - v[1]);
    }
    if (spread)
        *spread = widest;
    return {mid[0], mid[1], mid[2], mid[3]};
}

bool isDirt(const Rgba& cur, const std::vector<Rgba>& neighbours, float contrast, float motion)
{
    float spread = 0.f;
    const Rgba m = temporalMedian(neighbours, &spread);
    const float d = std::max({std::fabs(cur.r - m.r), std::fabs(cur.g - m.g),
        std::fabs(cur.b - m.b), std::fabs(cur.a - m.a)});
    return d > contrast && spread < motion;
}

std::array<float, 3> deflickerGain(
    const std::vector<Rgba>& means, bool perChannel, float strength, float limit)
{
    std::array<float, 3> gain {1.f, 1.f, 1.f};
    if (means.empty())
        return gain;
    float r = 0, g = 0, b = 0;
    for (const Rgba& m : means) {
        r += m.r;
        g += m.g;
        b += m.b;
    }
    const float n = float(means.size());
    r /= n;
    g /= n;
    b /= n;
    const Rgba& cur = means.front();
    const float lc = luma(cur.r, cur.g, cur.b);
    if (lc <= 1e-4f)
        return gain;
    if (perChannel) {
        gain = {r / std::max(cur.r, 1e-4f), g / std::max(cur.g, 1e-4f), b / std::max(cur.b, 1e-4f)};
    } else {
        const float x = luma(r, g, b) / lc;
        gain = {x, x, x};
    }
    return limited(gain, strength, limit);
}

std::array<float, 3> stabilizerGain(
    const Rgba& c, const Rgba& r, StabilizerMatch match, float strength, float limit)
{
    std::array<float, 3> gain {1.f, 1.f, 1.f};
    const float lc = luma(c.r, c.g, c.b), lr = luma(r.r, r.g, r.b);
    if (lc <= 1e-4f || lr <= 1e-4f)
        return gain;
    const std::array<float, 3> per {
        r.r / std::max(c.r, 1e-4f), r.g / std::max(c.g, 1e-4f), r.b / std::max(c.b, 1e-4f)};
    if (match == StabilizerMatch::Both) {
        gain = per;
    } else if (match == StabilizerMatch::Exposure) {
        gain = {lr / lc, lr / lc, lr / lc};
    } else {
        const float keep = lc / std::max(luma(c.r * per[0], c.g * per[1], c.b * per[2]), 1e-4f);
        gain = {per[0] * keep, per[1] * keep, per[2] * keep};
    }
    return limited(gain, strength, limit);
}

} // namespace kk::render::fxmath
