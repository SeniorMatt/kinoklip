// SPDX-License-Identifier: GPL-3.0-or-later
// EffectMathExtra — see EffectMathExtra.h.
#include "render/EffectMathExtra.h"

#include "core/VideoEffectKeys.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace kk::render::fxmath {

namespace {

namespace k = fxkeys;

float clamp01(float v)
{
    return std::clamp(v, 0.f, 1.f);
}

float value(const ParamSet& p, const QString& key, int64_t frame, double fallback)
{
    return float(p.value(key, frame, fallback));
}

using Mat3 = std::array<float, 9>; ///< row-major

/// RGB -> CIE XYZ (D65) of the gamuts the limiter knows (ITU-R BT.709, SMPTE EG 432-1 P3 with
/// the D65 white, ITU-R BT.2020), from their published primaries.
Mat3 toXyz(const QString& gamut)
{
    if (gamut == fxchoice::Rec2020)
        return {0.6369580f, 0.1446169f, 0.1688810f, 0.2627002f, 0.6779981f, 0.0593017f, 0.f,
            0.0280727f, 1.0609851f};
    if (gamut == fxchoice::DciP3)
        return {0.4865709f, 0.2656677f, 0.1982173f, 0.2289746f, 0.6917385f, 0.0792869f, 0.f,
            0.0451134f, 1.0439444f};
    return {0.4124564f, 0.3575761f, 0.1804375f, 0.2126729f, 0.7151522f, 0.0721750f, 0.0193339f,
        0.1191920f, 0.9503041f};
}

Mat3 multiply(const Mat3& a, const Mat3& b)
{
    Mat3 r {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int n = 0; n < 3; ++n)
                r[size_t(i * 3 + j)] += a[size_t(i * 3 + n)] * b[size_t(n * 3 + j)];
    return r;
}

Mat3 inverse(const Mat3& m)
{
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7],
                 i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const double s = 1.0 / det;
    return {float((e * i - f * h) * s), float((c * h - b * i) * s), float((b * f - c * e) * s),
        float((f * g - d * i) * s), float((a * i - c * g) * s), float((c * d - a * f) * s),
        float((d * h - e * g) * s), float((b * g - a * h) * s), float((a * e - b * d) * s)};
}

} // namespace

// ---- despill ----

DespillParams despillParams(const ParamSet& p, int64_t f)
{
    DespillParams r;
    const QString screen = p.staticValue(k::Screen).toString();
    r.channel = screen == fxchoice::Red ? 0 : (screen == fxchoice::Blue ? 2 : 1);
    r.amount = clamp01(value(p, k::Amount, f, 100) / 100.f);
    r.maximum = p.staticValue(k::SpillMethod).toString() == fxchoice::Maximum;
    return r;
}

Rgb despillColor(const DespillParams& p, const Rgb& c)
{
    float v[3] = {c.r, c.g, c.b};
    const float a = v[(p.channel + 1) % 3], b = v[(p.channel + 2) % 3];
    const float limit = p.maximum ? std::max(a, b) : 0.5f * (a + b);
    v[p.channel] -= p.amount * std::max(0.f, v[p.channel] - limit);
    return {v[0], v[1], v[2]};
}

// ---- gamut limiter ----

GamutLimiterParams gamutLimiterParams(const ParamSet& p, int64_t f)
{
    GamutLimiterParams r;
    const QString source = p.staticValue(k::SourceGamut).toString();
    const QString target = p.staticValue(k::TargetGamut).toString();
    const Mat3 src = toXyz(source.isEmpty() ? fxchoice::Rec2020 : source);
    const Mat3 dst = toXyz(target.isEmpty() ? fxchoice::Rec709 : target);
    const Mat3 to = multiply(inverse(dst), src);
    const Mat3 from = inverse(to);
    std::copy(to.begin(), to.end(), r.toTarget);
    std::copy(from.begin(), from.end(), r.fromTarget);
    for (int i = 0; i < 3; ++i)
        r.lumaTarget[i] = dst[size_t(3 + i)]; // the Y row of RGB -> XYZ
    r.gamma = std::clamp(value(p, k::Gamma, f, 2.4), 1.f, 3.f);
    r.desaturate = p.staticValue(k::Mode).toString() != fxchoice::Clip;
    r.amount = clamp01(value(p, k::Amount, f, 100) / 100.f);
    return r;
}

Rgb gamutLimit(const GamutLimiterParams& p, const Rgb& c)
{
    const float lin[3] = {std::pow(std::max(c.r, 0.f), p.gamma),
        std::pow(std::max(c.g, 0.f), p.gamma), std::pow(std::max(c.b, 0.f), p.gamma)};
    float t[3];
    for (int i = 0; i < 3; ++i)
        t[i] = p.toTarget[i * 3] * lin[0] + p.toTarget[i * 3 + 1] * lin[1]
            + p.toTarget[i * 3 + 2] * lin[2];
    if (t[0] >= 0.f && t[0] <= 1.f && t[1] >= 0.f && t[1] <= 1.f && t[2] >= 0.f && t[2] <= 1.f)
        return c;
    float r[3];
    if (p.desaturate) {
        const float y
            = clamp01(p.lumaTarget[0] * t[0] + p.lumaTarget[1] * t[1] + p.lumaTarget[2] * t[2]);
        float s = 1.f;
        for (int i = 0; i < 3; ++i) {
            if (t[i] < 0.f)
                s = std::min(s, y / (y - t[i]));
            else if (t[i] > 1.f)
                s = std::min(s, (1.f - y) / (t[i] - y));
        }
        for (int i = 0; i < 3; ++i)
            r[i] = y + s * (t[i] - y);
    } else {
        for (int i = 0; i < 3; ++i)
            r[i] = clamp01(t[i]);
    }
    float back[3];
    for (int i = 0; i < 3; ++i) {
        back[i] = clamp01(p.fromTarget[i * 3] * r[0] + p.fromTarget[i * 3 + 1] * r[1]
            + p.fromTarget[i * 3 + 2] * r[2]);
        back[i] = std::pow(back[i], 1.f / p.gamma);
    }
    return {c.r + (back[0] - c.r) * p.amount, c.g + (back[1] - c.g) * p.amount,
        c.b + (back[2] - c.b) * p.amount};
}

// ---- fast noise ----

Rgb blendNoise(int mode, float amount, const Rgb& b, const Rgb& n)
{
    auto one = [mode](float base, float noise) {
        switch (mode) {
        case 1:
            return base + noise;
        case 2:
            return base * noise;
        case 3:
            return 1.f - (1.f - base) * (1.f - noise);
        case 4:
            return base < 0.5f ? 2.f * base * noise : 1.f - 2.f * (1.f - base) * (1.f - noise);
        default:
            return noise;
        }
    };
    auto mixed = [&](float base, float noise) { return base + (one(base, noise) - base) * amount; };
    return {mixed(b.r, n.r), mixed(b.g, n.g), mixed(b.b, n.b)};
}

// ---- false color ----

namespace {

struct Stop
{
    float pos, r, g, b, grey;
};

/// Exposure bands: purple / blue / teal in the shadows, green and pink at the middle grey and
/// skin levels, yellow and red close to clipping; everything else keeps its grey.
constexpr Stop kExposure[] = {{0.00f, 0.40f, 0.00f, 0.55f, 0}, {0.02f, 0.00f, 0.15f, 0.85f, 0},
    {0.08f, 0.00f, 0.50f, 0.55f, 0}, {0.20f, 0, 0, 0, 1}, {0.42f, 0.15f, 0.70f, 0.20f, 0},
    {0.50f, 0, 0, 0, 1}, {0.62f, 0.95f, 0.60f, 0.70f, 0}, {0.72f, 0, 0, 0, 1},
    {0.92f, 0.95f, 0.85f, 0.10f, 0}, {0.98f, 0.95f, 0.10f, 0.10f, 0}};
constexpr Stop kThermal[]
    = {{0.0f, 0.0f, 0.0f, 0.0f, 0}, {0.2f, 0.1f, 0.0f, 0.4f, 0}, {0.4f, 0.6f, 0.0f, 0.5f, 0},
        {0.6f, 0.95f, 0.2f, 0.1f, 0}, {0.8f, 1.0f, 0.8f, 0.1f, 0}, {1.0f, 1.0f, 1.0f, 0.9f, 0}};
constexpr Stop kRainbow[]
    = {{0.0f, 0.3f, 0.0f, 0.5f, 0}, {0.2f, 0.0f, 0.2f, 1.0f, 0}, {0.4f, 0.0f, 0.9f, 0.9f, 0},
        {0.6f, 0.1f, 0.9f, 0.1f, 0}, {0.8f, 1.0f, 0.9f, 0.0f, 0}, {1.0f, 1.0f, 0.1f, 0.1f, 0}};

template<size_t N>
void fill(FalseColorParams& r, const Stop (&stops)[N])
{
    r.count = int(N);
    for (size_t i = 0; i < N; ++i) {
        r.pos[i] = stops[i].pos;
        r.rgb[i][0] = stops[i].r;
        r.rgb[i][1] = stops[i].g;
        r.rgb[i][2] = stops[i].b;
        r.grey[i] = stops[i].grey;
    }
}

} // namespace

FalseColorParams falseColorParams(const ParamSet& p, int64_t f)
{
    FalseColorParams r;
    const QString palette = p.staticValue(k::Palette).toString();
    if (palette == fxchoice::Thermal) {
        fill(r, kThermal);
    } else if (palette == fxchoice::Rainbow) {
        fill(r, kRainbow);
    } else {
        fill(r, kExposure);
        r.banded = true;
    }
    r.amount = clamp01(value(p, k::Amount, f, 100) / 100.f);
    return r;
}

Rgb falseColor(const FalseColorParams& p, const Rgb& c)
{
    const float y = clamp01(luma(c));
    float o[3];
    if (p.banded) {
        int i = 0;
        for (int n = 1; n < p.count; ++n)
            i = y >= p.pos[n] ? n : i;
        for (int j = 0; j < 3; ++j)
            o[j] = p.grey[i] > 0.5f ? y : p.rgb[i][j];
    } else {
        int i = 0;
        for (int n = 1; n < p.count - 1; ++n)
            i = y >= p.pos[n] ? n : i;
        const float t = clamp01((y - p.pos[i]) / (p.pos[i + 1] - p.pos[i]));
        for (int j = 0; j < 3; ++j)
            o[j] = p.rgb[i][j] + (p.rgb[i + 1][j] - p.rgb[i][j]) * t;
    }
    return {c.r + (o[0] - c.r) * p.amount, c.g + (o[1] - c.g) * p.amount,
        c.b + (o[2] - c.b) * p.amount};
}

namespace detail {

bool isExtraEffect(const QString& type)
{
    return type == QLatin1String(fxid::Despill) || type == QLatin1String(fxid::GamutLimiter)
        || type == QLatin1String(fxid::FalseColor);
}

bool applyExtraEffect(
    const QString& type, const ParamSet& params, int64_t localFrame, QImage& image)
{
    if (!isExtraEffect(type))
        return false;
    DespillParams despill;
    GamutLimiterParams gamut;
    FalseColorParams falsec;
    const bool isDespill = type == QLatin1String(fxid::Despill);
    const bool isFalse = type == QLatin1String(fxid::FalseColor);
    if (isDespill)
        despill = despillParams(params, localFrame);
    else if (isFalse)
        falsec = falseColorParams(params, localFrame);
    else
        gamut = gamutLimiterParams(params, localFrame);
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = line[x];
            const int a8 = qAlpha(px);
            if (a8 == 0)
                continue;
            const float alpha = a8 / 255.f;
            const float inv = 1.f / (255.f * alpha);
            const Rgb c {qRed(px) * inv, qGreen(px) * inv, qBlue(px) * inv};
            const Rgb r = isDespill ? despillColor(despill, c)
                : isFalse           ? falseColor(falsec, c)
                                    : gamutLimit(gamut, c);
            auto to8 = [alpha](float v) { return int(std::lround(clamp01(v) * alpha * 255.f)); };
            line[x] = qRgba(to8(r.r), to8(r.g), to8(r.b), a8);
        }
    }
    return true;
}

} // namespace detail

} // namespace kk::render::fxmath
