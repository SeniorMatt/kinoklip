// SPDX-License-Identifier: GPL-3.0-or-later
// EffectMathGrade — CPU reference of the second batch of pointwise effects (Curves, HSL
// Adjust, Apply LUT, Vibrance, Channel Mixer, Duotone, Flicker). Every formula has a GLSL
// twin in GlGradeEffects.cpp / GlTimeEffects.cpp; keep them in step.
#include "core/CurveParam.h"
#include "core/VideoEffectKeys.h"
#include "render/CubeLut.h"
#include "render/EffectMath.h"

#include <algorithm>
#include <cmath>
#include <memory>

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

Rgb mixRgb(const Rgb& a, const Rgb& b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

/// Hue of the skin tones (degrees) and the half width the vibrance spares them over.
constexpr float kSkinHue = 25.f;
constexpr float kSkinHalfWidth = 25.f;

/// The same integer hash as film grain's (GLSL twin in GlTimeEffects.cpp).
uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

} // namespace

Rgb fromHsl(float h, float s, float l)
{
    h = std::fmod(h, 360.f);
    if (h < 0.f)
        h += 360.f;
    const float c = (1.f - std::abs(2.f * l - 1.f)) * s;
    const float hp = h / 60.f;
    const float x = c * (1.f - std::abs(std::fmod(hp, 2.f) - 1.f));
    Rgb r;
    if (hp < 1.f)
        r = {c, x, 0.f};
    else if (hp < 2.f)
        r = {x, c, 0.f};
    else if (hp < 3.f)
        r = {0.f, c, x};
    else if (hp < 4.f)
        r = {0.f, x, c};
    else if (hp < 5.f)
        r = {x, 0.f, c};
    else
        r = {c, 0.f, x};
    const float m = l - 0.5f * c;
    return {r.r + m, r.g + m, r.b + m};
}

// ---- curves ----

void curvesParams(const ParamSet& p, int64_t localFrame, CurvesParams* out)
{
    const curve::Points master = curve::parse(p.staticValue(k::MasterCurve).toString());
    const curve::Points channels[3] = {curve::parse(p.staticValue(k::RedCurve).toString()),
        curve::parse(p.staticValue(k::GreenCurve).toString()),
        curve::parse(p.staticValue(k::BlueCurve).toString())};
    out->amount = clamp01(value(p, k::Amount, localFrame, 100) / 100.f);
    out->identity = curve::isIdentity(master) && curve::isIdentity(channels[0])
        && curve::isIdentity(channels[1]) && curve::isIdentity(channels[2]);
    float masterTable[kCurveTableSize];
    curve::sample(master, masterTable, kCurveTableSize);
    for (int c = 0; c < 3; ++c) {
        float own[kCurveTableSize];
        curve::sample(channels[c], own, kCurveTableSize);
        // Composite: the channel curve applied to the master curve's output.
        for (int i = 0; i < kCurveTableSize; ++i)
            out->table[c][i] = curveLookup(own, masterTable[i]);
    }
}

float curveLookup(const float* table, float x)
{
    const float t = clamp01(x) * float(kCurveTableSize - 1);
    const int i = std::min(int(t), kCurveTableSize - 2);
    const float f = t - float(i);
    return table[i] * (1.f - f) + table[i + 1] * f;
}

Rgb curves(const CurvesParams& p, const Rgb& c)
{
    const Rgb curved {
        curveLookup(p.table[0], c.r), curveLookup(p.table[1], c.g), curveLookup(p.table[2], c.b)};
    return mixRgb(c, curved, p.amount);
}

// ---- hsl_adjust ----

HslAdjustParams hslAdjustParams(const ParamSet& p, int64_t f)
{
    HslAdjustParams r;
    static const QString* const kRanges[] = {&fxchoice::Reds, &fxchoice::Yellows, &fxchoice::Greens,
        &fxchoice::Cyans, &fxchoice::Blues, &fxchoice::Magentas};
    const QString range = choiceParam(p, k::Range, fxchoice::All);
    r.all = true;
    for (int i = 0; i < 6; ++i) {
        if (range == *kRanges[i]) {
            r.all = false;
            r.centre = 60.f * float(i);
        }
    }
    r.halfWidth = 0.5f * std::max(1.f, value(p, k::HueWidth, f, 60));
    r.softness = std::max(1.f, value(p, k::HueSoftness, f, 30));
    r.hueShift = value(p, k::HueShift, f, 0);
    r.saturation = std::clamp(value(p, k::Saturation, f, 0) / 100.f, -1.f, 1.f);
    r.lightness = std::clamp(value(p, k::Lightness, f, 0) / 100.f, -1.f, 1.f);
    return r;
}

float hslRangeWeight(const HslAdjustParams& p, float h, float s)
{
    if (p.all)
        return 1.f;
    const float dh = std::abs(std::fmod(h - p.centre + 540.f, 360.f) - 180.f);
    // Greys have no hue: a range never selects them.
    return (1.f - smoothstep(p.halfWidth, p.halfWidth + p.softness, dh))
        * smoothstep(0.02f, 0.12f, s);
}

Rgb hslAdjust(const HslAdjustParams& p, const Rgb& c)
{
    float h = 0, s = 0, l = 0;
    toHsl(c, &h, &s, &l);
    const float w = hslRangeWeight(p, h, s);
    if (w <= 0.f)
        return c;
    h += p.hueShift * w;
    s = clamp01(s * (1.f + p.saturation * w));
    const float dl = p.lightness * w;
    l = dl >= 0.f ? l + (1.f - l) * dl : l * (1.f + dl);
    return fromHsl(h, s, l);
}

// ---- vibrance ----

VibranceParams vibranceParams(const ParamSet& p, int64_t f)
{
    return {std::clamp(value(p, k::Vibrance, f, 30) / 100.f, -1.f, 1.f),
        std::clamp(value(p, k::Saturation, f, 0) / 100.f, -1.f, 1.f)};
}

Rgb vibrance(const VibranceParams& p, const Rgb& c)
{
    const float y = luma(c);
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    const float sat = mx - mn;
    float h = 0, s = 0, l = 0;
    toHsl(c, &h, &s, &l);
    const float dh = std::abs(std::fmod(h - kSkinHue + 540.f, 360.f) - 180.f);
    const float skin = 1.f - smoothstep(0.f, kSkinHalfWidth, dh);
    // Positive vibrance boosts muted colours most and skin tones half as much.
    const float guard = p.vibrance > 0.f ? 1.f - 0.5f * skin : 1.f;
    const float k = (1.f + p.vibrance * (1.f - sat) * guard) * (1.f + p.saturation);
    return {y + (c.r - y) * k, y + (c.g - y) * k, y + (c.b - y) * k};
}

// ---- channel_mixer ----

ChannelMixerParams channelMixerParams(const ParamSet& p, int64_t f)
{
    ChannelMixerParams r;
    const QString* keys[9] = {&k::MixRR, &k::MixRG, &k::MixRB, &k::MixGR, &k::MixGG, &k::MixGB,
        &k::MixBR, &k::MixBG, &k::MixBB};
    const float defaults[9] = {100, 0, 0, 0, 100, 0, 0, 0, 100};
    for (int i = 0; i < 9; ++i)
        r.m[i] = value(p, *keys[i], f, defaults[i]) / 100.f;
    if (boolParam(p, k::Monochrome, false)) {
        for (int i = 3; i < 9; ++i)
            r.m[i] = r.m[i % 3];
    }
    return r;
}

Rgb channelMixer(const ChannelMixerParams& p, const Rgb& c)
{
    const float* m = p.m;
    return {m[0] * c.r + m[1] * c.g + m[2] * c.b, m[3] * c.r + m[4] * c.g + m[5] * c.b,
        m[6] * c.r + m[7] * c.g + m[8] * c.b};
}

// ---- duotone ----

DuotoneParams duotoneParams(const ParamSet& p, int64_t f)
{
    return {colorParam(p, k::ShadowColor, {0.f, 0.f, 0.f}),
        colorParam(p, k::HighlightColor, {1.f, 1.f, 1.f}),
        clamp01(value(p, k::Amount, f, 100) / 100.f)};
}

Rgb duotone(const DuotoneParams& p, const Rgb& c)
{
    const float y = clamp01(luma(c));
    return mixRgb(c, mixRgb(p.shadow, p.highlight, y), p.amount);
}

// ---- flicker ----

float valueNoise1d(double t, uint32_t seed)
{
    const double fl = std::floor(t);
    const int64_t i = int64_t(fl);
    float f = float(t - fl);
    f = f * f * (3.f - 2.f * f);
    auto lattice = [seed](int64_t n) {
        return float(hash32(uint32_t(n) * 0x9e3779b9u ^ hash32(seed + 0x51u))) / 4294967295.f * 2.f
            - 1.f;
    };
    const float a = lattice(i), b = lattice(i + 1);
    return a + (b - a) * f;
}

float flickerGain(const ParamSet& p, int64_t localFrame, double fps)
{
    const double t = fps > 0 ? double(localFrame) / fps : 0.0;
    const double freq = std::max(0.01, p.value(k::Frequency, localFrame, 8.0));
    const float amount = clamp01(value(p, k::Amount, localFrame, 30) / 100.f);
    const float random = clamp01(value(p, k::Randomness, localFrame, 70) / 100.f);
    const uint32_t seed = uint32_t(std::max(0.0, std::round(p.value(k::Seed, localFrame, 1.0))));
    constexpr double kTau = 6.283185307179586;
    const float wave = float(std::sin(kTau * freq * t));
    const float noise = valueNoise1d(freq * t, seed);
    return std::max(0.f, 1.f + amount * (wave * (1.f - random) + noise * random));
}

// ---- camera_shake ----

void cameraShake(
    const ParamSet& p, int64_t localFrame, double seconds, double* dx, double* dy, double* rotation)
{
    const double amplitude = std::max(0.0, p.value(k::Amplitude, localFrame, 12.0));
    const double freq = std::max(0.01, p.value(k::Frequency, localFrame, 4.0));
    const double rotationDeg = std::max(0.0, p.value(k::Rotation, localFrame, 1.0));
    const uint32_t seed
        = uint32_t(std::max(0.0, std::round(p.value(k::Seed, localFrame, 1.0)))) * 3u;
    const double t = seconds * freq;
    // Two octaves: the main sway plus a faster, smaller jitter.
    auto noise = [t](uint32_t s) {
        return 0.7 * valueNoise1d(t, s) + 0.3 * valueNoise1d(2.3 * t + 17.0, s + 101u);
    };
    *dx = amplitude * noise(seed);
    *dy = amplitude * noise(seed + 1u);
    *rotation = rotationDeg * 3.14159265358979 / 180.0 * noise(seed + 2u);
}

// ---- apply_lut ----

Rgb applyLut(const CubeLut& lut, float amount, const Rgb& c)
{
    float rgb[3] = {c.r, c.g, c.b};
    lut.apply(rgb);
    return mixRgb(c, {rgb[0], rgb[1], rgb[2]}, amount);
}

// ---- CPU fallback ----

namespace detail {

namespace {

/// Runs @p fn (straight Rgb -> Rgb) over every non-transparent pixel of @p image.
template<typename Fn>
void forEachPixel(QImage& image, Fn&& fn)
{
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = line[x];
            const int a8 = qAlpha(px);
            if (a8 == 0)
                continue;
            const float alpha = a8 / 255.f;
            const float inv = 1.f / (255.f * alpha);
            const Rgb c = fn(Rgb {qRed(px) * inv, qGreen(px) * inv, qBlue(px) * inv});
            auto to8 = [alpha](float v) { return int(std::lround(clamp01(v) * alpha * 255.f)); };
            line[x] = qRgba(to8(c.r), to8(c.g), to8(c.b), a8);
        }
    }
}

} // namespace

bool isGradeEffect(const QString& type)
{
    static const char* const kTypes[] = {fxid::Curves, fxid::HslAdjust, fxid::ApplyLut,
        fxid::Vibrance, fxid::ChannelMixer, fxid::Duotone, fxid::Flicker};
    for (const char* t : kTypes) {
        if (type == QLatin1String(t))
            return true;
    }
    return false;
}

bool applyGradeEffect(
    const QString& type, const ParamSet& params, int64_t localFrame, QImage& image, double fps)
{
    if (type == QLatin1String(fxid::Curves)) {
        auto p = std::make_unique<CurvesParams>();
        curvesParams(params, localFrame, p.get());
        forEachPixel(image, [&p](const Rgb& c) { return curves(*p, c); });
    } else if (type == QLatin1String(fxid::HslAdjust)) {
        const HslAdjustParams p = hslAdjustParams(params, localFrame);
        forEachPixel(image, [&p](const Rgb& c) { return hslAdjust(p, c); });
    } else if (type == QLatin1String(fxid::ApplyLut)) {
        const std::shared_ptr<const CubeLut> lut
            = loadCubeLut(params.staticValue(k::LutFile).toString());
        if (!lut)
            return false;
        const float amount = clamp01(value(params, k::Amount, localFrame, 100) / 100.f);
        forEachPixel(image, [&](const Rgb& c) { return applyLut(*lut, amount, c); });
    } else if (type == QLatin1String(fxid::Vibrance)) {
        const VibranceParams p = vibranceParams(params, localFrame);
        forEachPixel(image, [&p](const Rgb& c) { return vibrance(p, c); });
    } else if (type == QLatin1String(fxid::ChannelMixer)) {
        const ChannelMixerParams p = channelMixerParams(params, localFrame);
        forEachPixel(image, [&p](const Rgb& c) { return channelMixer(p, c); });
    } else if (type == QLatin1String(fxid::Duotone)) {
        const DuotoneParams p = duotoneParams(params, localFrame);
        forEachPixel(image, [&p](const Rgb& c) { return duotone(p, c); });
    } else if (type == QLatin1String(fxid::Flicker)) {
        const float gain = flickerGain(params, localFrame, fps);
        forEachPixel(
            image, [gain](const Rgb& c) { return Rgb {c.r * gain, c.g * gain, c.b * gain}; });
    } else {
        return false;
    }
    return true;
}

} // namespace detail

} // namespace kk::render::fxmath
