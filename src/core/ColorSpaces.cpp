// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSpaces — see ColorSpaces.h.
#include "core/ColorSpaces.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace kk::cm {

namespace {

struct Primaries
{
    Chromaticity r, g, b, w;
};

constexpr Chromaticity kD65 {0.3127, 0.3290};
constexpr Chromaticity kD60 {0.32168, 0.33767};

constexpr Entry kGamutEntries[] = {
    {"rec709", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Rec.709 / sRGB")},
    {"rec2020", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Rec.2020")},
    {"p3d65", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "P3-D65")},
    {"p3dci", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "P3-DCI")},
    {"p3d60", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "P3-D60")},
    {"acesap0", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "ACES AP0 (ACES2065-1)")},
    {"acesap1", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "ACES AP1 (ACEScg)")},
    {"adobergb", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Adobe RGB")},
    {"davinciwide", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "DaVinci Wide Gamut")},
    {"alexawide", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "ARRI Wide Gamut 3")},
    {"sgamut3", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Sony S-Gamut3")},
    {"sgamut3cine", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Sony S-Gamut3.Cine")},
    {"vgamut", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Panasonic V-Gamut")},
    {"redwide", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "REDWideGamutRGB")},
};
static_assert(std::size(kGamutEntries) == size_t(Gamut::Count));

constexpr Entry kTransferEntries[] = {
    {"linear", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Linear")},
    {"gamma22", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Gamma 2.2")},
    {"gamma24", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Gamma 2.4")},
    {"gamma26", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Gamma 2.6")},
    {"srgb", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "sRGB")},
    {"rec709", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Rec.709")},
    {"pq", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "ST2084 (PQ)")},
    {"hlg", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "HLG")},
    {"acescc", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "ACEScc")},
    {"acescct", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "ACEScct")},
    {"davinci", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "DaVinci Intermediate")},
    {"logc3", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "ARRI LogC3")},
    {"slog3", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Sony S-Log3")},
    {"vlog", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Panasonic V-Log")},
    {"log3g10", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "RED Log3G10")},
};
static_assert(std::size(kTransferEntries) == size_t(Transfer::Count));

constexpr Entry kWhiteEntries[] = {
    {"d50", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "D50")},
    {"d55", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "D55")},
    {"d60", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "D60 (ACES)")},
    {"d65", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "D65")},
    {"dci", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "DCI")},
    {"d75", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "D75")},
    {"a", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Illuminant A (tungsten)")},
    {"e", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Equal energy (E)")},
    {"custom", QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", "Custom (x, y)")},
};
static_assert(std::size(kWhiteEntries) == size_t(WhitePoint::Count));

#define KK_ACES(id, label, gamut, tf, display)                                          \
    {                                                                                    \
        id, QT_TRANSLATE_NOOP("kk::BuiltinVideoEffects", label), Gamut::gamut,          \
            Transfer::tf, display                                                        \
    }
constexpr AcesEntry kAcesEntries[] = {
    KK_ACES("aces2065", "ACES2065-1 (AP0 linear)", AcesAp0, Linear, false),
    KK_ACES("acescg", "ACEScg", AcesAp1, Linear, false),
    KK_ACES("acescc", "ACEScc", AcesAp1, AcesCc, false),
    KK_ACES("acescct", "ACEScct", AcesAp1, AcesCct, false),
    KK_ACES("srgb", "sRGB", Rec709, Srgb, true),
    KK_ACES("rec709", "Rec.709 (BT.1886)", Rec709, Gamma24, true),
    KK_ACES("rec2020", "Rec.2020 (BT.1886)", Rec2020, Gamma24, true),
    KK_ACES("p3d65", "P3-D65 (gamma 2.6)", P3D65, Gamma26, true),
    KK_ACES("rec2020pq", "Rec.2020 ST2084 (no tone map)", Rec2020, Pq, false),
    KK_ACES("rec2020hlg", "Rec.2020 HLG (no tone map)", Rec2020, Hlg, false),
    KK_ACES("linear709", "Linear Rec.709", Rec709, Linear, false),
    KK_ACES("linear2020", "Linear Rec.2020", Rec2020, Linear, false),
    KK_ACES("davinci", "DaVinci Wide Gamut / Intermediate", DavinciWide, DavinciIntermediate, false),
    KK_ACES("logc3", "ARRI Wide Gamut 3 / LogC3", AlexaWide, LogC3, false),
    KK_ACES("slog3", "Sony S-Gamut3.Cine / S-Log3", SGamut3Cine, SLog3, false),
    KK_ACES("vlog", "Panasonic V-Gamut / V-Log", VGamut, VLog, false),
    KK_ACES("log3g10", "REDWideGamutRGB / Log3G10", RedWide, Log3G10, false),
};
#undef KK_ACES

Primaries primariesOf(Gamut g)
{
    switch (g) {
    case Gamut::Rec2020:
        return {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, kD65};
    case Gamut::P3D65:
        return {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, kD65};
    case Gamut::P3Dci:
        return {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.314, 0.351}};
    case Gamut::P3D60:
        return {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, kD60};
    case Gamut::AcesAp0:
        return {{0.7347, 0.2653}, {0.0, 1.0}, {0.0001, -0.0770}, kD60};
    case Gamut::AcesAp1:
        return {{0.713, 0.293}, {0.165, 0.830}, {0.128, 0.044}, kD60};
    case Gamut::AdobeRgb:
        return {{0.64, 0.33}, {0.21, 0.71}, {0.15, 0.06}, kD65};
    case Gamut::DavinciWide:
        return {{0.8000, 0.3130}, {0.1682, 0.9877}, {0.0790, -0.1155}, kD65};
    case Gamut::AlexaWide:
        return {{0.6840, 0.3130}, {0.2210, 0.8480}, {0.0861, -0.1020}, kD65};
    case Gamut::SGamut3:
        return {{0.730, 0.280}, {0.140, 0.855}, {0.100, -0.050}, kD65};
    case Gamut::SGamut3Cine:
        return {{0.766, 0.275}, {0.225, 0.800}, {0.089, -0.087}, kD65};
    case Gamut::VGamut:
        return {{0.730, 0.280}, {0.165, 0.840}, {0.100, -0.030}, kD65};
    case Gamut::RedWide:
        return {{0.780308, 0.304253}, {0.121595, 1.493994}, {0.095612, -0.084589}, kD65};
    case Gamut::Rec709:
    case Gamut::Count:
        break;
    }
    return {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, kD65};
}

Vec3 column(const Mat3& m, int c)
{
    return {m[size_t(c)], m[size_t(3 + c)], m[size_t(6 + c)]};
}

float hlgOetfInverse(float e)
{
    constexpr float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    if (e <= 0.5f)
        return e * e / 3.f;
    return (std::exp((e - c) / a) + b) / 12.f;
}

float hlgOetf(float s)
{
    constexpr float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    if (s <= 1.f / 12.f)
        return std::sqrt(std::max(3.f * s, 0.f));
    return a * std::log(12.f * s - b) + c;
}

// PQ (SMPTE ST 2084) constants.
constexpr double kPqM1 = 2610.0 / 16384.0;
constexpr double kPqM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kPqC1 = 3424.0 / 4096.0;
constexpr double kPqC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kPqC3 = 2392.0 / 4096.0 * 32.0;

} // namespace

const Entry& gamutEntry(Gamut g)
{
    return kGamutEntries[size_t(g)];
}
const Entry& transferEntry(Transfer t)
{
    return kTransferEntries[size_t(t)];
}
const Entry& whitePointEntry(WhitePoint w)
{
    return kWhiteEntries[size_t(w)];
}

int acesEntryCount()
{
    return int(std::size(kAcesEntries));
}
const AcesEntry& acesEntry(int i)
{
    return kAcesEntries[size_t(std::clamp(i, 0, acesEntryCount() - 1))];
}
const AcesEntry* acesEntryFromId(const QString& id)
{
    for (const AcesEntry& e : kAcesEntries)
        if (id == QLatin1String(e.id))
            return &e;
    return nullptr;
}

Gamut gamutFromId(const QString& id, Gamut fallback)
{
    for (int i = 0; i < int(Gamut::Count); ++i)
        if (id == QLatin1String(kGamutEntries[i].id))
            return Gamut(i);
    return fallback;
}
Transfer transferFromId(const QString& id, Transfer fallback)
{
    for (int i = 0; i < int(Transfer::Count); ++i)
        if (id == QLatin1String(kTransferEntries[i].id))
            return Transfer(i);
    return fallback;
}
WhitePoint whitePointFromId(const QString& id, WhitePoint fallback)
{
    for (int i = 0; i < int(WhitePoint::Count); ++i)
        if (id == QLatin1String(kWhiteEntries[i].id))
            return WhitePoint(i);
    return fallback;
}

// ---- gamuts ----

Chromaticity whiteOf(Gamut g)
{
    return primariesOf(g).w;
}

Chromaticity whiteOf(WhitePoint w, Chromaticity custom)
{
    switch (w) {
    case WhitePoint::D50:
        return {0.34567, 0.35850};
    case WhitePoint::D55:
        return {0.33242, 0.34743};
    case WhitePoint::D60:
        return kD60;
    case WhitePoint::Dci:
        return {0.314, 0.351};
    case WhitePoint::D75:
        return {0.29902, 0.31485};
    case WhitePoint::IlluminantA:
        return {0.44757, 0.40745};
    case WhitePoint::IlluminantE:
        return {1.0 / 3.0, 1.0 / 3.0};
    case WhitePoint::Custom:
        return {std::clamp(custom.x, 0.01, 0.8), std::clamp(custom.y, 0.01, 0.9)};
    case WhitePoint::D65:
    case WhitePoint::Count:
        break;
    }
    return kD65;
}

Vec3 xyzOf(Chromaticity c)
{
    return {c.x / c.y, 1.0, (1.0 - c.x - c.y) / c.y};
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
    return {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, (f * g - d * i) * s,
        (a * i - c * g) * s, (c * d - a * f) * s, (d * h - e * g) * s, (b * g - a * h) * s,
        (a * e - b * d) * s};
}

Vec3 mulVec(const Mat3& m, const Vec3& v)
{
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
        m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Mat3 rgbToXyz(Gamut g)
{
    const Primaries p = primariesOf(g);
    const Vec3 r = xyzOf(p.r), gr = xyzOf(p.g), b = xyzOf(p.b);
    const Mat3 prim {r[0], gr[0], b[0], r[1], gr[1], b[1], r[2], gr[2], b[2]};
    const Vec3 s = mulVec(inverse(prim), xyzOf(p.w));
    return {prim[0] * s[0], prim[1] * s[1], prim[2] * s[2], prim[3] * s[0], prim[4] * s[1],
        prim[5] * s[2], prim[6] * s[0], prim[7] * s[1], prim[8] * s[2]};
}

Mat3 xyzToRgb(Gamut g)
{
    return inverse(rgbToXyz(g));
}

Vec3 lumaWeights(Gamut g)
{
    const Mat3 m = rgbToXyz(g);
    return {m[3], m[4], m[5]};
}

Mat3 coneMatrix(Cat cat)
{
    switch (cat) {
    case Cat::Cat02:
        return {0.7328, 0.4296, -0.1624, -0.7036, 1.6975, 0.0061, 0.0030, 0.0136, 0.9834};
    case Cat::VonKries: // Hunt-Pointer-Estevez, normalised to D65
        return {0.4002, 0.7076, -0.0808, -0.2263, 1.1653, 0.0457, 0.0, 0.0, 0.9182};
    case Cat::Bradford:
        break;
    }
    return {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
}

Mat3 adaptation(Cat cat, Chromaticity from, Chromaticity to)
{
    const Mat3 m = coneMatrix(cat);
    const Vec3 cs = mulVec(m, xyzOf(from)), cd = mulVec(m, xyzOf(to));
    const Mat3 scale {cd[0] / cs[0], 0, 0, 0, cd[1] / cs[1], 0, 0, 0, cd[2] / cs[2]};
    return multiply(inverse(m), multiply(scale, m));
}

Mat3 gamutMatrix(Gamut from, Gamut to, Cat cat, bool adapt)
{
    Mat3 xyz = rgbToXyz(from);
    if (adapt)
        xyz = multiply(adaptation(cat, whiteOf(from), whiteOf(to)), xyz);
    return multiply(xyzToRgb(to), xyz);
}

// ---- transfer functions ----

bool isDisplayReferred(Transfer t)
{
    return t == Transfer::Pq || t == Transfer::Hlg;
}

float decode(Transfer t, float x)
{
    switch (t) {
    case Transfer::Linear:
        return x;
    case Transfer::Gamma22:
        return std::pow(std::max(x, 0.f), 2.2f);
    case Transfer::Gamma24:
        return std::pow(std::max(x, 0.f), 2.4f);
    case Transfer::Gamma26:
        return std::pow(std::max(x, 0.f), 2.6f);
    case Transfer::Srgb:
        return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    case Transfer::Rec709:
        return x < 0.081f ? x / 4.5f : std::pow((x + 0.099f) / 1.099f, 1.f / 0.45f);
    case Transfer::Pq: {
        const double e = std::pow(std::max(double(x), 0.0), 1.0 / kPqM2);
        const double y = std::pow(std::max(e - kPqC1, 0.0) / (kPqC2 - kPqC3 * e), 1.0 / kPqM1);
        return float(y * 100.0); // 10000 nits = 100 units of 100 nits
    }
    case Transfer::Hlg:
        return hlgOetfInverse(x);
    case Transfer::AcesCc:
        if (x < (9.72f - 15.f) / 17.52f)
            return (std::exp2(x * 17.52f - 9.72f) - std::exp2(-16.f)) * 2.f;
        return std::min(std::exp2(x * 17.52f - 9.72f), 65504.f);
    case Transfer::AcesCct:
        if (x <= 0.155251141552511f)
            return (x - 0.0729055341958355f) / 10.5402377416545f;
        return std::min(std::exp2(x * 17.52f - 9.72f), 65504.f);
    case Transfer::DavinciIntermediate: {
        constexpr float a = 0.0075f, b = 7.f, c = 0.07329248f, m = 10.44426855f,
                        logCut = 0.02740668f;
        return x <= logCut ? x / m : std::exp2(x / c - b) - a;
    }
    case Transfer::LogC3: {
        constexpr float cut = 0.010591f, a = 5.555556f, b = 0.052272f, c = 0.247190f,
                        d = 0.385537f, e = 5.367655f, f = 0.092809f;
        if (x > e * cut + f)
            return (std::pow(10.f, (x - d) / c) - b) / a;
        return (x - f) / e;
    }
    case Transfer::SLog3:
        if (x >= 171.2102946929f / 1023.f)
            return std::pow(10.f, (x * 1023.f - 420.f) / 261.5f) * (0.18f + 0.01f) - 0.01f;
        return (x * 1023.f - 95.f) * 0.01125f / (171.2102946929f - 95.f);
    case Transfer::VLog: {
        constexpr float b = 0.00873f, c = 0.241514f, d = 0.598206f;
        if (x < 0.181f)
            return (x - 0.125f) / 5.6f;
        return std::pow(10.f, (x - d) / c) - b;
    }
    case Transfer::Log3G10: {
        constexpr float a = 0.224282f, b = 155.975327f, c = 0.01f, g = 15.1927f;
        if (x < 0.f)
            return x / g - c;
        return (std::pow(10.f, x / a) - 1.f) / b - c;
    }
    case Transfer::Count:
        break;
    }
    return x;
}

float encode(Transfer t, float x)
{
    switch (t) {
    case Transfer::Linear:
        return x;
    case Transfer::Gamma22:
        return std::pow(std::max(x, 0.f), 1.f / 2.2f);
    case Transfer::Gamma24:
        return std::pow(std::max(x, 0.f), 1.f / 2.4f);
    case Transfer::Gamma26:
        return std::pow(std::max(x, 0.f), 1.f / 2.6f);
    case Transfer::Srgb:
        return x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f;
    case Transfer::Rec709:
        return x < 0.018f ? x * 4.5f : 1.099f * std::pow(x, 0.45f) - 0.099f;
    case Transfer::Pq: {
        const double y = std::pow(std::clamp(double(x) / 100.0, 0.0, 1.0), kPqM1);
        return float(std::pow((kPqC1 + kPqC2 * y) / (1.0 + kPqC3 * y), kPqM2));
    }
    case Transfer::Hlg:
        return hlgOetf(std::max(x, 0.f));
    case Transfer::AcesCc:
        if (x <= 0.f)
            return (-16.f + 9.72f) / 17.52f;
        if (x < std::exp2(-15.f))
            return (std::log2(std::exp2(-16.f) + x * 0.5f) + 9.72f) / 17.52f;
        return (std::log2(x) + 9.72f) / 17.52f;
    case Transfer::AcesCct:
        if (x <= 0.0078125f)
            return 10.5402377416545f * x + 0.0729055341958355f;
        return (std::log2(x) + 9.72f) / 17.52f;
    case Transfer::DavinciIntermediate: {
        constexpr float a = 0.0075f, b = 7.f, c = 0.07329248f, m = 10.44426855f,
                        linCut = 0.00262409f;
        return x > linCut ? (std::log2(x + a) + b) * c : x * m;
    }
    case Transfer::LogC3: {
        constexpr float cut = 0.010591f, a = 5.555556f, b = 0.052272f, c = 0.247190f,
                        d = 0.385537f, e = 5.367655f, f = 0.092809f;
        return x > cut ? c * std::log10(a * x + b) + d : e * x + f;
    }
    case Transfer::SLog3:
        if (x >= 0.01125f)
            return (420.f + std::log10((x + 0.01f) / (0.18f + 0.01f)) * 261.5f) / 1023.f;
        return (x * (171.2102946929f - 95.f) / 0.01125f + 95.f) / 1023.f;
    case Transfer::VLog: {
        constexpr float b = 0.00873f, c = 0.241514f, d = 0.598206f;
        return x < 0.01f ? 5.6f * x + 0.125f : c * std::log10(x + b) + d;
    }
    case Transfer::Log3G10: {
        constexpr float a = 0.224282f, b = 155.975327f, c = 0.01f, g = 15.1927f;
        const float v = x + c;
        return v < 0.f ? v * g : a * std::log10(v * b + 1.f);
    }
    case Transfer::Count:
        break;
    }
    return x;
}

// ---- pipeline ----

float hlgGamma(float nits)
{
    return 1.2f + 0.42f * std::log10(std::max(nits, 1.f) / 1000.f);
}

float compress(float x, float thr, float lim, float power)
{
    if (x < thr)
        return x;
    lim = std::max(lim, 1.0001f);
    thr = std::min(thr, 0.9999f);
    const float s
        = (lim - thr) / std::pow(std::pow((1.f - thr) / (lim - thr), -power) - 1.f, 1.f / power);
    const float u = (x - thr) / s;
    return thr + s * u / std::pow(1.f + std::pow(u, power), 1.f / power);
}

void compressGamut(float rgb[3], float thr, float lim, float power)
{
    const float ach = std::max({rgb[0], rgb[1], rgb[2]});
    if (ach <= 0.f) {
        // Nothing positive to measure against: negative channels simply go to zero.
        for (int i = 0; i < 3; ++i)
            rgb[i] = std::max(rgb[i], 0.f);
        return;
    }
    for (int i = 0; i < 3; ++i) {
        const float d = (ach - rgb[i]) / ach;
        rgb[i] = ach - compress(d, thr, lim, power) * ach;
    }
}

namespace {

void multiply3(const float* m, const float* v, float* out)
{
    for (int i = 0; i < 3; ++i)
        out[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
}

float dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void toneMap(const CstParams& p, float t[3])
{
    if (p.tone == int(ToneMap::None) || p.peakIn <= p.peakOut)
        return;
    const float pout = p.peakOut;
    auto shoulder = [&](float x) {
        switch (ToneMap(p.tone)) {
        case ToneMap::Davinci: {
            const float knee = 0.5f * pout;
            return x <= knee ? x : knee + (pout - knee) * std::tanh((x - knee) / (pout - knee));
        }
        default: {
            const float w = p.peakIn / pout;
            const float n = x / pout;
            return pout * n * (1.f + n / (w * w)) / (1.f + n);
        }
        }
    };
    if (p.tone == int(ToneMap::Simple)) {
        for (int i = 0; i < 3; ++i)
            t[i] = t[i] > 0.f ? shoulder(t[i]) : t[i];
        return;
    }
    const float y = dot3(p.lumaOut, t);
    if (y <= 1e-6f)
        return;
    const float k = shoulder(y) / y;
    for (int i = 0; i < 3; ++i)
        t[i] *= k;
}

void saturationMap(const CstParams& p, float t[3])
{
    const float lo = std::min({t[0], t[1], t[2]});
    if (lo >= 0.f)
        return;
    const float y = std::max(dot3(p.lumaOut, t), 0.f);
    const float s = y / (y - lo);
    for (int i = 0; i < 3; ++i)
        t[i] = y + s * (t[i] - y);
}

} // namespace

CstParams makeCst(Gamut inGamut, Transfer inTf, Gamut outGamut, Transfer outTf, ToneMap tone,
    GamutMap gmap, bool fwdOotf, bool invOotf, bool adapt, double inNits, double outNits, Cat cat)
{
    CstParams p;
    const Mat3 m = gamutMatrix(inGamut, outGamut, cat, adapt);
    for (size_t i = 0; i < 9; ++i)
        p.m[i] = float(m[i]);
    p.inTf = int(inTf);
    p.outTf = int(outTf);
    p.tone = int(tone);
    p.gmap = int(gmap);
    p.peakIn = float(std::max(inNits, 1.0) / 100.0);
    p.peakOut = float(std::max(outNits, 1.0) / 100.0);
    p.fwdOotf = fwdOotf;
    p.invOotf = invOotf;
    const Vec3 li = lumaWeights(inGamut), lo = lumaWeights(outGamut);
    for (size_t i = 0; i < 3; ++i) {
        p.lumaIn[i] = float(li[i]);
        p.lumaOut[i] = float(lo[i]);
    }
    return p;
}

namespace {

void setMat(float* dst, const Mat3& m)
{
    for (size_t i = 0; i < 9; ++i)
        dst[i] = float(m[i]);
}

} // namespace

CstParams makeAces(const AcesEntry& input, const AcesEntry& output, bool invert)
{
    CstParams p;
    p.inTf = int(input.transfer);
    p.outTf = int(output.transfer);
    // Everything goes through ACEScg (AP1): the RRT + ODT fit works there.
    setMat(p.m, gamutMatrix(input.gamut, Gamut::AcesAp1, Cat::Bradford, true));
    setMat(p.m2, gamutMatrix(Gamut::AcesAp1, output.gamut, Cat::Bradford, true));
    if (invert && input.display)
        p.rrt = 2;
    else if (!invert && output.display)
        p.rrt = 1;
    if (p.rrt == 1) {
        // The display matrix applies after the fit, whose output is display-linear in AP1.
        setMat(p.m2, gamutMatrix(Gamut::AcesAp1, output.gamut, Cat::Bradford, true));
    } else if (p.rrt == 2) {
        // The input is display light in its own gamut: AP1 only after the inverse fit.
        setMat(p.m, gamutMatrix(input.gamut, Gamut::AcesAp1, Cat::Bradford, true));
    }
    const Vec3 li = lumaWeights(input.gamut), lo = lumaWeights(output.gamut);
    for (size_t i = 0; i < 3; ++i) {
        p.lumaIn[i] = float(li[i]);
        p.lumaOut[i] = float(lo[i]);
    }
    return p;
}

CstParams makeGamutMapping(Gamut gamut, Transfer tf, Gamut target, GamutMap mode, double threshold,
    double limit, double power, double amount)
{
    CstParams p;
    p.inTf = p.outTf = int(tf);
    setMat(p.m, gamutMatrix(gamut, target, Cat::Bradford, true));
    setMat(p.m3, gamutMatrix(target, gamut, Cat::Bradford, true));
    p.gmap = int(mode);
    p.gmThr = float(threshold);
    p.gmLim = float(limit);
    p.gmPwr = float(power);
    p.amount = float(std::clamp(amount, 0.0, 1.0));
    const Vec3 lo = lumaWeights(target);
    for (size_t i = 0; i < 3; ++i)
        p.lumaOut[i] = float(lo[i]);
    return p;
}

CstParams makeAdaptation(
    Gamut gamut, Transfer tf, Cat cat, Chromaticity from, Chromaticity to, double amount)
{
    CstParams p;
    p.inTf = p.outTf = int(tf);
    setMat(p.m,
        multiply(xyzToRgb(gamut), multiply(adaptation(cat, from, to), rgbToXyz(gamut))));
    p.amount = float(std::clamp(amount, 0.0, 1.0));
    return p;
}

CstParams makeCompressor(Gamut gamut, Transfer tf, double hiThr, double hiLim, double satThr,
    double satLim, double amount)
{
    CstParams p;
    p.inTf = p.outTf = int(tf);
    p.hiThr = float(hiThr);
    p.hiLim = float(hiLim);
    p.gmap = int(GamutMap::Compression);
    p.gmThr = float(satThr);
    p.gmLim = float(satLim);
    p.amount = float(std::clamp(amount, 0.0, 1.0));
    const Vec3 lo = lumaWeights(gamut);
    for (size_t i = 0; i < 3; ++i)
        p.lumaOut[i] = float(lo[i]);
    return p;
}

void cstApply(const CstParams& p, const float in[3], float out[3])
{
    float lin[3];
    for (int i = 0; i < 3; ++i)
        lin[i] = decode(Transfer(p.inTf), in[i]);
    if (p.fwdOotf && p.inTf == int(Transfer::Hlg)) {
        // Rec. 2100 HLG OOTF: scene light to display light of a peakIn display.
        const float ys = std::max(dot3(p.lumaIn, lin), 0.f);
        const float k = p.peakIn * std::pow(ys, hlgGamma(p.peakIn * 100.f) - 1.f);
        for (float& v : lin)
            v *= k;
    }
    float t[3];
    multiply3(p.m, lin, t);
    if (p.rrt == 1) {
        float d[3];
        rrtOdtFit(t, d);
        for (int i = 0; i < 3; ++i)
            t[i] = d[i];
    } else if (p.rrt == 2) {
        float d[3];
        rrtOdtFitInverse(t, d);
        for (int i = 0; i < 3; ++i)
            t[i] = d[i];
    }
    {
        float u[3] {t[0], t[1], t[2]};
        multiply3(p.m2, u, t);
    }
    if (p.rrt == 1) {
        for (float& v : t)
            v = std::clamp(v, 0.f, 1.f);
    }
    if (p.hiLim > 0.f) {
        for (float& v : t)
            v = compress(v, p.hiThr, p.hiLim, p.hiPwr);
    }
    toneMap(p, t);
    if (p.gmap == int(GamutMap::Saturation))
        saturationMap(p, t);
    else if (p.gmap == int(GamutMap::Compression))
        compressGamut(t, p.gmThr, p.gmLim, p.gmPwr);
    if (p.invOotf && p.outTf == int(Transfer::Hlg)) {
        const float g = hlgGamma(p.peakOut * 100.f);
        const float yd = std::max(dot3(p.lumaOut, t), 0.f);
        const float ys = std::pow(yd / p.peakOut, 1.f / g);
        const float k = ys > 0.f ? 1.f / (p.peakOut * std::pow(ys, g - 1.f)) : 0.f;
        for (float& v : t)
            v *= k;
    }
    {
        float u[3] {t[0], t[1], t[2]};
        multiply3(p.m3, u, t);
    }
    for (int i = 0; i < 3; ++i)
        out[i] = in[i] + (encode(Transfer(p.outTf), t[i]) - in[i]) * p.amount;
}

// ---- ACES RRT + ODT fit ----

namespace {

// The matrices of the published fit, which take linear sRGB (D65) to the space the curve works
// in and back (S. Hill, "ACES Filmic Tone Mapping Curve", MIT licence; the polynomial is K.
// Narkowicz's refit of the ACES RRT + ODT).
constexpr Mat3 kFitIn {0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383,
    0.83777};
constexpr Mat3 kFitOut {1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327,
    -0.07276, 1.07602};

struct FitMatrices
{
    Mat3 toFit, fromFit, toFitInv, fromFitInv;
    FitMatrices()
    {
        // Re-based on ACEScg so that any display gamut can follow: AP1 -> fit space and back.
        toFit = multiply(kFitIn, gamutMatrix(Gamut::AcesAp1, Gamut::Rec709, Cat::Bradford, true));
        fromFit = multiply(gamutMatrix(Gamut::Rec709, Gamut::AcesAp1, Cat::Bradford, true), kFitOut);
        toFitInv = inverse(toFit);
        fromFitInv = inverse(fromFit);
    }
};

const FitMatrices& fitMatrices()
{
    static const FitMatrices m;
    return m;
}

} // namespace

void fitMatrices(float toFit[9], float fromFit[9], float toFitInv[9], float fromFitInv[9])
{
    const FitMatrices& fm = fitMatrices();
    for (size_t i = 0; i < 9; ++i) {
        toFit[i] = float(fm.toFit[i]);
        fromFit[i] = float(fm.fromFit[i]);
        toFitInv[i] = float(fm.toFitInv[i]);
        fromFitInv[i] = float(fm.fromFitInv[i]);
    }
}

void rrtOdtFit(const float in[3], float out[3])
{
    const FitMatrices& fm = fitMatrices();
    float v[3];
    for (int i = 0; i < 3; ++i)
        v[i] = float(fm.toFit[size_t(i * 3)] * in[0] + fm.toFit[size_t(i * 3 + 1)] * in[1]
            + fm.toFit[size_t(i * 3 + 2)] * in[2]);
    for (float& x : v)
        x = (x * (x + 0.0245786f) - 0.000090537f) / (x * (0.983729f * x + 0.4329510f) + 0.238081f);
    for (int i = 0; i < 3; ++i)
        out[i] = float(fm.fromFit[size_t(i * 3)] * v[0] + fm.fromFit[size_t(i * 3 + 1)] * v[1]
            + fm.fromFit[size_t(i * 3 + 2)] * v[2]);
}

void rrtOdtFitInverse(const float in[3], float out[3])
{
    const FitMatrices& fm = fitMatrices();
    float v[3];
    for (int i = 0; i < 3; ++i)
        v[i] = float(fm.fromFitInv[size_t(i * 3)] * in[0] + fm.fromFitInv[size_t(i * 3 + 1)] * in[1]
            + fm.fromFitInv[size_t(i * 3 + 2)] * in[2]);
    for (float& y : v) {
        y = std::clamp(y, 0.f, 0.9999f);
        const float a = 0.983729f * y - 1.f, b = 0.4329510f * y - 0.0245786f,
                    c = 0.238081f * y + 0.000090537f;
        y = (-b - std::sqrt(std::max(b * b - 4.f * a * c, 0.f))) / (2.f * a);
    }
    for (int i = 0; i < 3; ++i)
        out[i] = float(fm.toFitInv[size_t(i * 3)] * v[0] + fm.toFitInv[size_t(i * 3 + 1)] * v[1]
            + fm.toFitInv[size_t(i * 3 + 2)] * v[2]);
}

} // namespace kk::cm
