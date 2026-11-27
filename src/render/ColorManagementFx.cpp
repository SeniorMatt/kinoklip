// SPDX-License-Identifier: GPL-3.0-or-later
// ColorManagementFx — see ColorManagementFx.h.
#include "render/ColorManagementFx.h"

#include "core/VideoEffectKeys.h"

#include <QImage>

#include <algorithm>
#include <cmath>

namespace kk::render::fxmath {

namespace {

namespace k = fxkeys;
namespace c = fxchoice;

QString text(const ParamSet& p, const QString& key, const char* fallback)
{
    const QString v = p.staticValue(key).toString();
    return v.isEmpty() ? QString::fromLatin1(fallback) : v;
}

double num(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return p.value(key, f, fallback);
}

cm::Gamut gamutOf(const ParamSet& p, const QString& key, cm::Gamut fallback)
{
    return cm::gamutFromId(p.staticValue(key).toString(), fallback);
}

cm::Transfer transferOf(const ParamSet& p, const QString& key, cm::Transfer fallback)
{
    return cm::transferFromId(p.staticValue(key).toString(), fallback);
}

cm::Cat catOf(const ParamSet& p)
{
    const QString m = p.staticValue(k::CmMethod).toString();
    if (m == c::CatCat02)
        return cm::Cat::Cat02;
    if (m == c::CatVonKries)
        return cm::Cat::VonKries;
    return cm::Cat::Bradford;
}

cm::Chromaticity whiteParam(const ParamSet& p, const QString& preset, const QString& xKey,
    const QString& yKey, int64_t f, cm::WhitePoint fallback)
{
    const cm::WhitePoint w = cm::whitePointFromId(p.staticValue(preset).toString(), fallback);
    return cm::whiteOf(w, {num(p, xKey, f, 0.3127), num(p, yKey, f, 0.3290)});
}

} // namespace

bool isColorManagementEffect(const QString& type)
{
    return type == QLatin1String(fxid::ColorSpaceTransform)
        || type == QLatin1String(fxid::AcesTransform) || type == QLatin1String(fxid::GamutMapping)
        || type == QLatin1String(fxid::ChromaticAdaptation)
        || type == QLatin1String(fxid::ColorCompressor);
}

cm::CstParams colorManagementParams(const QString& type, const ParamSet& p, int64_t f)
{
    if (type == QLatin1String(fxid::AcesTransform)) {
        const cm::AcesEntry* in = cm::acesEntryFromId(text(p, k::AcesInput, "srgb"));
        const cm::AcesEntry* out = cm::acesEntryFromId(text(p, k::AcesOutput, "acescg"));
        return cm::makeAces(in ? *in : cm::acesEntry(4), out ? *out : cm::acesEntry(1),
            boolParam(p, k::AcesInvert, false));
    }
    if (type == QLatin1String(fxid::GamutMapping)) {
        const bool sat = p.staticValue(k::CmGmMode).toString() == c::GmSaturation;
        return cm::makeGamutMapping(gamutOf(p, k::CmInGamut, cm::Gamut::Rec2020),
            transferOf(p, k::CmInGamma, cm::Transfer::Gamma24),
            gamutOf(p, k::CmTarget, cm::Gamut::Rec709),
            sat ? cm::GamutMap::Saturation : cm::GamutMap::Compression,
            num(p, k::CmThreshold, f, 80) / 100.0, num(p, k::CmLimit, f, 120) / 100.0,
            num(p, k::CmPower, f, 1.2), num(p, k::Amount, f, 100) / 100.0);
    }
    if (type == QLatin1String(fxid::ChromaticAdaptation)) {
        return cm::makeAdaptation(gamutOf(p, k::CmInGamut, cm::Gamut::Rec709),
            transferOf(p, k::CmInGamma, cm::Transfer::Gamma24), catOf(p),
            whiteParam(p, k::CmSrcWhite, k::CmSrcX, k::CmSrcY, f, cm::WhitePoint::D65),
            whiteParam(p, k::CmDstWhite, k::CmDstX, k::CmDstY, f, cm::WhitePoint::D50),
            num(p, k::Amount, f, 100) / 100.0);
    }
    if (type == QLatin1String(fxid::ColorCompressor)) {
        return cm::makeCompressor(gamutOf(p, k::CmInGamut, cm::Gamut::Rec709),
            transferOf(p, k::CmInGamma, cm::Transfer::Gamma24),
            num(p, k::CmHiThreshold, f, 80) / 100.0, num(p, k::CmHiLimit, f, 200) / 100.0,
            num(p, k::CmSatThreshold, f, 80) / 100.0, num(p, k::CmSatLimit, f, 120) / 100.0,
            num(p, k::Amount, f, 100) / 100.0);
    }
    // Color Space Transform
    cm::ToneMap tone = cm::ToneMap::None;
    const QString t = p.staticValue(k::CmTone).toString();
    if (t == c::ToneSimple)
        tone = cm::ToneMap::Simple;
    else if (t == c::ToneLuminance)
        tone = cm::ToneMap::Luminance;
    else if (t == c::ToneDavinci)
        tone = cm::ToneMap::Davinci;
    cm::GamutMap gmap = cm::GamutMap::None;
    const QString g = p.staticValue(k::CmGamutMap).toString();
    if (g == c::GmSaturation)
        gmap = cm::GamutMap::Saturation;
    else if (g == c::GmCompression)
        gmap = cm::GamutMap::Compression;
    return cm::makeCst(gamutOf(p, k::CmInGamut, cm::Gamut::DavinciWide),
        transferOf(p, k::CmInGamma, cm::Transfer::DavinciIntermediate),
        gamutOf(p, k::CmOutGamut, cm::Gamut::Rec709),
        transferOf(p, k::CmOutGamma, cm::Transfer::Gamma24), tone, gmap,
        boolParam(p, k::CmFwdOotf, false), boolParam(p, k::CmInvOotf, false),
        boolParam(p, k::CmAdapt, true), num(p, k::CmMaxIn, f, 1000), num(p, k::CmMaxOut, f, 100));
}

bool colorManagementIdentity(const QString& type, const ParamSet& p, int64_t f)
{
    if (type != QLatin1String(fxid::ColorSpaceTransform)
        && type != QLatin1String(fxid::AcesTransform) && num(p, k::Amount, f, 100) <= 0.0)
        return true;
    if (type == QLatin1String(fxid::ChromaticAdaptation)) {
        const cm::CstParams cp = colorManagementParams(type, p, f);
        float d = 0.f;
        for (int i = 0; i < 9; ++i)
            d = std::max(d, std::abs(cp.m[i] - ((i % 4 == 0) ? 1.f : 0.f)));
        return d < 1e-6f;
    }
    return false;
}

bool applyColorManagementEffect(
    const QString& type, const ParamSet& p, int64_t localFrame, QImage& image)
{
    if (!isColorManagementEffect(type))
        return false;
    const cm::CstParams cp = colorManagementParams(type, p, localFrame);
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = line[x];
            const int a8 = qAlpha(px);
            if (a8 == 0)
                continue;
            const float alpha = a8 / 255.f;
            const float inv = 1.f / (255.f * alpha);
            const float in[3] = {qRed(px) * inv, qGreen(px) * inv, qBlue(px) * inv};
            float out[3];
            cm::cstApply(cp, in, out);
            auto to8 = [alpha](float v) {
                return int(std::lround(std::clamp(v, 0.f, 1.f) * alpha * 255.f));
            };
            line[x] = qRgba(to8(out[0]), to8(out[1]), to8(out[2]), a8);
        }
    }
    return true;
}

} // namespace kk::render::fxmath
