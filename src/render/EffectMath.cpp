// SPDX-License-Identifier: GPL-3.0-or-later
// EffectMath — CPU reference of the v4 pointwise effects. Every formula here has a twin in a
// GLSL shader (GlKeyers.cpp, GlColorEffects.cpp, GlStylizeEffects.cpp); keep them in step.
#include "render/EffectMath.h"

#include "core/VideoEffectKeys.h"
#include "render/EffectMathExtra.h"

#include <QColor>

#include <algorithm>
#include <cmath>

namespace kk::render::fxmath {

namespace {

/// Luma offset of the chroma plane normalisation (see chromaDistance()): keeps near-black
/// pixels from exploding while making the plane mostly brightness-independent.
constexpr float kChromaLumaBias = 0.1f;
/// Smallest key chroma length used as the distance unit (a grey key colour stays finite).
constexpr float kMinKeyChroma = 0.05f;
/// Smallest ramp width: smoothstep needs e1 > e0.
constexpr float kMinRamp = 1e-3f;
/// Display gamma of the linear-light approximations (exposure, temperature).
constexpr float kDisplayGamma = 2.2f;
/// Film grain strength at Amount 100 %.
constexpr float kGrainScale = 0.4f;

float clamp01(float v)
{
    return std::clamp(v, 0.f, 1.f);
}

float mix(float a, float b, float t)
{
    return a + (b - a) * t;
}

Rgb mix(const Rgb& a, const Rgb& b, float t)
{
    return {mix(a.r, b.r, t), mix(a.g, b.g, t), mix(a.b, b.b, t)};
}

Rgb clamp01(const Rgb& c)
{
    return {clamp01(c.r), clamp01(c.g), clamp01(c.b)};
}

float value(const ParamSet& p, const QString& key, int64_t frame, double fallback)
{
    return float(p.value(key, frame, fallback));
}

/// Chroma plane position of @p c (CbCr / (Y + bias)).
void chromaOf(const Rgb& c, float* u, float* v)
{
    const float y = luma(c);
    const float scale = 1.f / (y + kChromaLumaBias);
    *u = (c.b - y) / 1.8556f * scale; // BT.709 Cb
    *v = (c.r - y) / 1.5748f * scale; // BT.709 Cr
}

/// Lower / upper soft range test: 1 inside [lo, hi], ramps of width @p soft outside.
float rangeMask(float lo, float hi, float soft, float x)
{
    const float s = std::max(soft, kMinRamp);
    return smoothstep(lo - s, lo, x) * (1.f - smoothstep(hi, hi + s, x));
}

uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float latticeValue(int x, int y, uint32_t seed)
{
    const uint32_t h = hash(uint32_t(x) + hash(uint32_t(y) + hash(seed)));
    return float(h & 0xffffffU) / 16777215.f - 0.5f;
}

} // namespace

float smoothstep(float e0, float e1, float x)
{
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.f - 2.f * t);
}

float luma(const Rgb& c)
{
    return kLumaR * c.r + kLumaG * c.g + kLumaB * c.b;
}

// ---- Shared helpers ----

QString choiceParam(const ParamSet& p, const QString& key, const QString& fallback)
{
    const auto it = p.statics.constFind(key);
    return it == p.statics.constEnd() ? fallback : it->toString();
}

bool boolParam(const ParamSet& p, const QString& key, bool fallback)
{
    const auto it = p.statics.constFind(key);
    return it == p.statics.constEnd() ? fallback : it->toBool();
}

Rgb colorParam(const ParamSet& p, const QString& key, const Rgb& fallback)
{
    const auto it = p.statics.constFind(key);
    if (it == p.statics.constEnd())
        return fallback;
    const QColor c = it->value<QColor>();
    if (!c.isValid())
        return fallback;
    return {float(c.redF()), float(c.greenF()), float(c.blueF())};
}

KeyView keyView(const ParamSet& p)
{
    const QString v = choiceParam(p, fxkeys::View, fxchoice::Final);
    if (v == fxchoice::Matte)
        return KeyView::Matte;
    if (v == fxchoice::Status)
        return KeyView::Status;
    return KeyView::Final;
}

// ---- Keyers ----

Rgb screenPresetColor(bool blue)
{
    // Typical chroma key paint colours (generic values, not a manufacturer's).
    return blue ? Rgb {0.f, 0x47 / 255.f, 0xbb / 255.f} : Rgb {0.f, 0xb1 / 255.f, 0x40 / 255.f};
}

ChromaKeyParams chromaKeyParams(const ParamSet& p, int64_t f)
{
    namespace k = fxkeys;
    ChromaKeyParams r;
    const QString screen = choiceParam(p, k::Screen, fxchoice::Green);
    if (screen == fxchoice::Custom)
        r.key = colorParam(p, k::KeyColor, screenPresetColor(false));
    else
        r.key = screenPresetColor(screen == fxchoice::Blue);
    r.tolerance = value(p, k::Tolerance, f, 30) / 100.f;
    r.softness = value(p, k::Softness, f, 20) / 100.f;
    r.clipBlack = value(p, k::ClipBlack, f, 0) / 100.f;
    r.clipWhite = value(p, k::ClipWhite, f, 100) / 100.f;
    r.invert = boolParam(p, k::Invert, false);
    r.spillAmount = std::clamp(value(p, k::SpillAmount, f, 50) / 100.f, 0.f, 1.f);
    r.spillMaximum = choiceParam(p, k::SpillMethod, fxchoice::Average) == fxchoice::Maximum;
    r.spillChannel = screen == fxchoice::Blue ? 2
        : screen == fxchoice::Green           ? 1
        : r.key.g >= r.key.b                  ? 1
                                              : 2;
    r.shrinkGrowPx = value(p, k::ShrinkGrow, f, 0);
    r.edgeBlurPx = std::max(0.f, value(p, k::EdgeBlur, f, 0));
    r.view = keyView(p);
    return r;
}

float chromaDistance(const ChromaKeyParams& p, const Rgb& c)
{
    float ku = 0, kv = 0, cu = 0, cv = 0;
    chromaOf(p.key, &ku, &kv);
    chromaOf(c, &cu, &cv);
    const float keyLength = std::max(std::hypot(ku, kv), kMinKeyChroma);
    const float du = ku / keyLength, dv = kv / keyLength; // key direction (unit unless grey)
    const float along = (cu * du + cv * dv) / keyLength; // 1 = as saturated as the key
    const float pu = cu - (cu * du + cv * dv) * du;
    const float pv = cv - (cu * du + cv * dv) * dv;
    const float across = std::hypot(pu, pv) / keyLength;
    const float short_ = std::max(0.f, 1.f - along);
    return std::sqrt(across * across + short_ * short_);
}

float chromaKeyMatte(const ChromaKeyParams& p, const Rgb& c)
{
    const float d = chromaDistance(p, c);
    float m = smoothstep(p.tolerance, p.tolerance + std::max(p.softness, kMinRamp), d);
    m = clamp01((m - p.clipBlack) / std::max(p.clipWhite - p.clipBlack, kMinRamp));
    return p.invert ? 1.f - m : m;
}

Rgb despill(const ChromaKeyParams& p, const Rgb& c)
{
    Rgb r = c;
    if (p.spillChannel == 2) {
        const float limit = p.spillMaximum ? std::max(c.r, c.g) : 0.5f * (c.r + c.g);
        r.b = c.b - p.spillAmount * std::max(0.f, c.b - limit);
    } else {
        const float limit = p.spillMaximum ? std::max(c.r, c.b) : 0.5f * (c.r + c.b);
        r.g = c.g - p.spillAmount * std::max(0.f, c.g - limit);
    }
    return r;
}

LumaKeyParams lumaKeyParams(const ParamSet& p, int64_t f)
{
    namespace k = fxkeys;
    LumaKeyParams r;
    r.low = value(p, k::Low, f, 20) / 100.f;
    r.high = value(p, k::High, f, 100) / 100.f;
    r.softness = value(p, k::Softness, f, 10) / 100.f;
    r.invert = boolParam(p, k::Invert, false);
    r.view = keyView(p);
    return r;
}

float lumaKeyMatte(const LumaKeyParams& p, const Rgb& c)
{
    const float m = rangeMask(p.low, p.high, p.softness, luma(c));
    return p.invert ? 1.f - m : m;
}

HslKeyParams hslKeyParams(const ParamSet& p, int64_t f)
{
    namespace k = fxkeys;
    HslKeyParams r;
    r.hue = value(p, k::Hue, f, 120);
    r.hueWidth = value(p, k::HueWidth, f, 60);
    r.hueSoftness = value(p, k::HueSoftness, f, 20);
    r.satLow = value(p, k::SatLow, f, 20) / 100.f;
    r.satHigh = value(p, k::SatHigh, f, 100) / 100.f;
    r.satSoftness = value(p, k::SatSoftness, f, 10) / 100.f;
    r.lumLow = value(p, k::LumLow, f, 0) / 100.f;
    r.lumHigh = value(p, k::LumHigh, f, 100) / 100.f;
    r.lumSoftness = value(p, k::LumSoftness, f, 10) / 100.f;
    r.invert = boolParam(p, k::Invert, false);
    r.view = keyView(p);
    return r;
}

void toHsl(const Rgb& c, float* h, float* s, float* l)
{
    const float mx = std::max({c.r, c.g, c.b});
    const float mn = std::min({c.r, c.g, c.b});
    const float d = mx - mn;
    *l = 0.5f * (mx + mn);
    *s = d <= 1e-6f ? 0.f : d / std::max(1.f - std::abs(2.f * *l - 1.f), 1e-6f);
    float hue = 0.f;
    if (d > 1e-6f) {
        if (mx == c.r)
            hue = std::fmod((c.g - c.b) / d + 6.f, 6.f);
        else if (mx == c.g)
            hue = (c.b - c.r) / d + 2.f;
        else
            hue = (c.r - c.g) / d + 4.f;
    }
    *h = hue * 60.f;
}

float hslKeyMatte(const HslKeyParams& p, const Rgb& c)
{
    float h = 0, s = 0, l = 0;
    toHsl(c, &h, &s, &l);
    // Circular hue distance 0..180.
    const float dh = std::abs(std::fmod(h - p.hue + 540.f, 360.f) - 180.f);
    const float half = 0.5f * p.hueWidth;
    const float qh = 1.f - smoothstep(half, half + std::max(p.hueSoftness, kMinRamp), dh);
    const float qs = rangeMask(p.satLow, p.satHigh, p.satSoftness, s);
    const float ql = rangeMask(p.lumLow, p.lumHigh, p.lumSoftness, l);
    const float m = 1.f - qh * qs * ql;
    return p.invert ? 1.f - m : m;
}

void keyOutput(KeyView view, const Rgb& c, float alpha, float matte, float out[4])
{
    const float a = alpha * matte;
    switch (view) {
    case KeyView::Matte:
        out[0] = out[1] = out[2] = a;
        out[3] = 1.f;
        return;
    case KeyView::Status: {
        const float v = a >= 0.98f ? 1.f : a <= 0.02f ? 0.f : 0.5f;
        out[0] = out[1] = out[2] = v;
        out[3] = 1.f;
        return;
    }
    case KeyView::Final:
        break;
    }
    out[0] = clamp01(c.r) * a;
    out[1] = clamp01(c.g) * a;
    out[2] = clamp01(c.b) * a;
    out[3] = a;
}

// ---- Colour ----

ExposureParams exposureParams(const ParamSet& p, int64_t f)
{
    ExposureParams r;
    r.gain = std::exp2(value(p, fxkeys::Exposure, f, 0));
    r.invGamma = 1.f / std::max(value(p, fxkeys::Gamma, f, 1), 0.01f);
    return r;
}

Rgb exposure(const ExposureParams& p, const Rgb& c)
{
    auto one = [&](float v) {
        const float lin = std::pow(std::max(v, 0.f), kDisplayGamma) * p.gain;
        return std::pow(clamp01(std::pow(lin, 1.f / kDisplayGamma)), p.invGamma);
    };
    return {one(c.r), one(c.g), one(c.b)};
}

GainParams temperatureTintParams(const ParamSet& p, int64_t f)
{
    // Warmer = more red, less blue; positive tint = magenta (less green). The gains are
    // normalised so a grey keeps its brightness.
    constexpr float kStrength = 0.3f;
    const float t = value(p, fxkeys::Temperature, f, 0) / 100.f;
    const float n = value(p, fxkeys::Tint, f, 0) / 100.f;
    GainParams r {1.f + kStrength * t, 1.f - kStrength * n, 1.f - kStrength * t};
    const float l = kLumaR * r.r + kLumaG * r.g + kLumaB * r.b;
    r.r /= l;
    r.g /= l;
    r.b /= l;
    return r;
}

Rgb linearGains(const GainParams& p, const Rgb& c)
{
    auto one = [](float v, float gain) {
        const float lin = std::pow(std::max(v, 0.f), kDisplayGamma) * gain;
        return std::pow(clamp01(lin), 1.f / kDisplayGamma);
    };
    return {one(c.r, p.r), one(c.g, p.g), one(c.b, p.b)};
}

LevelsParams levelsParams(const ParamSet& p, int64_t f)
{
    LevelsParams r;
    r.inBlack = value(p, fxkeys::InBlack, f, 0) / 255.f;
    r.inWhite = value(p, fxkeys::InWhite, f, 255) / 255.f;
    r.invGamma = 1.f / std::max(value(p, fxkeys::Gamma, f, 1), 0.01f);
    r.outBlack = value(p, fxkeys::OutBlack, f, 0) / 255.f;
    r.outWhite = value(p, fxkeys::OutWhite, f, 255) / 255.f;
    return r;
}

Rgb levels(const LevelsParams& p, const Rgb& c)
{
    const float range = std::max(p.inWhite - p.inBlack, 1.f / 255.f);
    auto one = [&](float v) {
        const float t = std::pow(clamp01((v - p.inBlack) / range), p.invGamma);
        return p.outBlack + t * (p.outWhite - p.outBlack);
    };
    return {one(c.r), one(c.g), one(c.b)};
}

Rgb invert(float amount, const Rgb& c)
{
    return mix(c, Rgb {1.f - c.r, 1.f - c.g, 1.f - c.b}, amount);
}

BlackWhiteParams blackWhiteParams(const ParamSet& p, int64_t f)
{
    BlackWhiteParams r;
    r.amount = std::clamp(value(p, fxkeys::Amount, f, 100) / 100.f, 0.f, 1.f);
    const Rgb tint = colorParam(p, fxkeys::TintColor, Rgb {0xc8 / 255.f, 0xa0 / 255.f, 0.44f});
    const float l = std::max(luma(tint), 1e-3f);
    r.tint = {tint.r / l, tint.g / l, tint.b / l};
    r.tintAmount = std::clamp(value(p, fxkeys::TintAmount, f, 0) / 100.f, 0.f, 1.f);
    return r;
}

Rgb blackWhite(const BlackWhiteParams& p, const Rgb& c)
{
    const float y = luma(c);
    const Rgb grey {y, y, y};
    const Rgb tinted = clamp01(Rgb {y * p.tint.r, y * p.tint.g, y * p.tint.b});
    return mix(c, mix(grey, tinted, p.tintAmount), p.amount);
}

Rgb posterize(int levels, const Rgb& c)
{
    const float n = float(std::max(levels, 2));
    auto one = [n](float v) { return std::min(std::floor(clamp01(v) * n), n - 1.f) / (n - 1.f); };
    return {one(c.r), one(c.g), one(c.b)};
}

ColorBalanceParams colorBalanceParams(const ParamSet& p, int64_t f)
{
    namespace k = fxkeys;
    ColorBalanceParams r;
    const float lift = value(p, k::Lift, f, 0);
    const float gamma = value(p, k::Gamma, f, 0);
    const float gain = value(p, k::Gain, f, 100) / 100.f;
    const QString* liftKeys[3] = {&k::LiftR, &k::LiftG, &k::LiftB};
    const QString* gammaKeys[3] = {&k::GammaR, &k::GammaG, &k::GammaB};
    const QString* gainKeys[3] = {&k::GainR, &k::GainG, &k::GainB};
    for (int i = 0; i < 3; ++i) {
        // Lift ±100 (+ channel ±100) → ±0.5 (±1) of the shadows; gamma ±100 → exponent
        // 2^±1 (positive brightens the midtones); gain multiplies.
        r.lift[i] = (lift + value(p, *liftKeys[i], f, 0)) / 200.f;
        r.invGamma[i] = 1.f / std::exp2((gamma + value(p, *gammaKeys[i], f, 0)) / 100.f);
        r.gain[i] = gain * (1.f + value(p, *gainKeys[i], f, 0) / 100.f);
    }
    return r;
}

Rgb colorBalance(const ColorBalanceParams& p, const Rgb& c)
{
    auto one = [&](float v, int i) {
        const float t = p.gain[i] * (v + p.lift[i] * (1.f - v));
        return std::pow(clamp01(t), p.invGamma[i]);
    };
    return {one(c.r, 0), one(c.g, 1), one(c.b, 2)};
}

float grainNoise(float x, float y, float cellPx, uint32_t seed)
{
    const float gx = x / cellPx, gy = y / cellPx;
    const float fx = std::floor(gx), fy = std::floor(gy);
    const int ix = int(fx), iy = int(fy);
    float tx = gx - fx, ty = gy - fy;
    tx = tx * tx * (3.f - 2.f * tx);
    ty = ty * ty * (3.f - 2.f * ty);
    const float a = latticeValue(ix, iy, seed), b = latticeValue(ix + 1, iy, seed);
    const float c = latticeValue(ix, iy + 1, seed), d = latticeValue(ix + 1, iy + 1, seed);
    return mix(mix(a, b, tx), mix(c, d, tx), ty);
}

// ---- CPU fallback ----

bool isPointEffect(const QString& type)
{
    static const char* const kTypes[]
        = {fxid::ChromaKey, fxid::LumaKey, fxid::HslKey, fxid::Exposure, fxid::TemperatureTint,
            fxid::Levels, fxid::Invert, fxid::BlackWhite, fxid::Posterize, fxid::ColorBalance};
    for (const char* t : kTypes) {
        if (type == QLatin1String(t))
            return true;
    }
    return detail::isGradeEffect(type) || detail::isExtraEffect(type);
}

bool applyPointEffect(
    const QString& type, const ParamSet& params, int64_t localFrame, QImage& image, double fps)
{
    if (!isPointEffect(type) || image.isNull())
        return false;
    if (image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (detail::isGradeEffect(type))
        return detail::applyGradeEffect(type, params, localFrame, image, fps);
    if (detail::isExtraEffect(type))
        return detail::applyExtraEffect(type, params, localFrame, image);

    // One resolved parameter set per call; the per-pixel lambda only does arithmetic.
    enum class Kind {
        Chroma,
        Luma,
        Hsl,
        Colour
    };
    Kind kind = Kind::Colour;
    ChromaKeyParams chroma;
    LumaKeyParams lumaKey;
    HslKeyParams hsl;
    ExposureParams expo;
    GainParams gains;
    LevelsParams lv;
    BlackWhiteParams bw;
    ColorBalanceParams cb;
    float amount = 1.f;
    int steps = 6;
    int colour = 0;
    if (type == QLatin1String(fxid::ChromaKey)) {
        kind = Kind::Chroma;
        chroma = chromaKeyParams(params, localFrame);
    } else if (type == QLatin1String(fxid::LumaKey)) {
        kind = Kind::Luma;
        lumaKey = lumaKeyParams(params, localFrame);
    } else if (type == QLatin1String(fxid::HslKey)) {
        kind = Kind::Hsl;
        hsl = hslKeyParams(params, localFrame);
    } else if (type == QLatin1String(fxid::Exposure)) {
        expo = exposureParams(params, localFrame);
        colour = 0;
    } else if (type == QLatin1String(fxid::TemperatureTint)) {
        gains = temperatureTintParams(params, localFrame);
        colour = 1;
    } else if (type == QLatin1String(fxid::Levels)) {
        lv = levelsParams(params, localFrame);
        colour = 2;
    } else if (type == QLatin1String(fxid::Invert)) {
        amount = std::clamp(float(params.value(fxkeys::Amount, localFrame, 100)) / 100.f, 0.f, 1.f);
        colour = 3;
    } else if (type == QLatin1String(fxid::BlackWhite)) {
        bw = blackWhiteParams(params, localFrame);
        colour = 4;
    } else if (type == QLatin1String(fxid::Posterize)) {
        steps = int(std::lround(params.value(fxkeys::Levels, localFrame, 6)));
        colour = 5;
    } else {
        cb = colorBalanceParams(params, localFrame);
        colour = 6;
    }

    // Transparent pixels stay transparent, except in a keyer's opaque Matte / Status views.
    const KeyView view = kind == Kind::Chroma ? chroma.view
        : kind == Kind::Luma                  ? lumaKey.view
        : kind == Kind::Hsl                   ? hsl.view
                                              : KeyView::Final;
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = line[x];
            const int a8 = qAlpha(px);
            if (a8 == 0 && view == KeyView::Final)
                continue;
            const float alpha = a8 / 255.f;
            const float inv = a8 ? 1.f / (255.f * alpha) : 0.f;
            const Rgb c {qRed(px) * inv, qGreen(px) * inv, qBlue(px) * inv};
            float out[4];
            switch (kind) {
            case Kind::Chroma:
                keyOutput(chroma.view, despill(chroma, c), alpha, chromaKeyMatte(chroma, c), out);
                break;
            case Kind::Luma:
                keyOutput(lumaKey.view, c, alpha, lumaKeyMatte(lumaKey, c), out);
                break;
            case Kind::Hsl:
                keyOutput(hsl.view, c, alpha, hslKeyMatte(hsl, c), out);
                break;
            case Kind::Colour: {
                Rgb r;
                switch (colour) {
                case 0:
                    r = exposure(expo, c);
                    break;
                case 1:
                    r = linearGains(gains, c);
                    break;
                case 2:
                    r = levels(lv, c);
                    break;
                case 3:
                    r = invert(amount, c);
                    break;
                case 4:
                    r = blackWhite(bw, c);
                    break;
                case 5:
                    r = posterize(steps, c);
                    break;
                default:
                    r = colorBalance(cb, c);
                    break;
                }
                r = clamp01(r);
                out[0] = r.r * alpha;
                out[1] = r.g * alpha;
                out[2] = r.b * alpha;
                out[3] = alpha;
                break;
            }
            }
            auto to8 = [](float v) { return int(std::lround(clamp01(v) * 255.f)); };
            line[x] = qRgba(to8(out[0]), to8(out[1]), to8(out[2]), to8(out[3]));
        }
    }
    return true;
}

} // namespace kk::render::fxmath
