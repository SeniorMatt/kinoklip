// SPDX-License-Identifier: GPL-3.0-or-later
// The colour management effects of the Color folder (docs/behavior-spec.md section 205): Color
// Space Transform, ACES Transform, Gamut Mapping, Chromatic Adaptation, Color Compressor and
// DCTL. The maths is in core/ColorSpaces.h; the pixels are drawn by render/GlColorManagement.cpp
// (GPU) and render/ColorManagementFx.cpp (CPU).
#include "core/ColorSpaces.h"
#include "core/EffectBuilders.h"
#include "core/EffectRegistry.h"
#include "core/VideoEffectKeys.h"

#include <QCoreApplication>

namespace kk {

using namespace builders;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::BuiltinVideoEffects", text);
}

const QString kPercent = QStringLiteral("%");
const QString kNits = QStringLiteral("nits");

ParamDescriptor gamutChoice(const QString& key, const QString& label, cm::Gamut def)
{
    QStringList ids, labels;
    for (int i = 0; i < int(cm::Gamut::Count); ++i) {
        const cm::Entry& e = cm::gamutEntry(cm::Gamut(i));
        ids << QLatin1String(e.id);
        labels << tr(e.label);
    }
    return choice(key, label, ids, labels, QLatin1String(cm::gamutEntry(def).id));
}

ParamDescriptor gammaChoice(const QString& key, const QString& label, cm::Transfer def)
{
    QStringList ids, labels;
    for (int i = 0; i < int(cm::Transfer::Count); ++i) {
        const cm::Entry& e = cm::transferEntry(cm::Transfer(i));
        ids << QLatin1String(e.id);
        labels << tr(e.label);
    }
    return choice(key, label, ids, labels, QLatin1String(cm::transferEntry(def).id));
}

ParamDescriptor whiteChoice(const QString& key, const QString& label, cm::WhitePoint def)
{
    QStringList ids, labels;
    for (int i = 0; i < int(cm::WhitePoint::Count); ++i) {
        const cm::Entry& e = cm::whitePointEntry(cm::WhitePoint(i));
        ids << QLatin1String(e.id);
        labels << tr(e.label);
    }
    return choice(key, label, ids, labels, QLatin1String(cm::whitePointEntry(def).id));
}

QStringList acesIds()
{
    QStringList r;
    for (int i = 0; i < cm::acesEntryCount(); ++i)
        r << QLatin1String(cm::acesEntry(i).id);
    return r;
}

QStringList acesLabels()
{
    QStringList r;
    for (int i = 0; i < cm::acesEntryCount(); ++i)
        r << tr(cm::acesEntry(i).label);
    return r;
}

EffectDescriptor effect(const char* id, const QString& name, const char* group,
    const QString& description, QVector<ParamDescriptor> params)
{
    EffectDescriptor d = descriptor(
        QLatin1String(id), name, EffectCategory::VideoEffect, description, std::move(params));
    d.group = QLatin1String(group);
    return d;
}

ParamDescriptor percent(
    const QString& key, const QString& label, double min, double max, double def)
{
    return number(key, label, min, max, def, 0.5, kPercent);
}

ParamDescriptor methodChoice()
{
    namespace c = fxchoice;
    return choice(fxkeys::CmMethod, tr("Method"), {c::CatBradford, c::CatCat02, c::CatVonKries},
        {tr("Bradford"), tr("CAT02"), tr("Von Kries")}, c::CatBradford);
}

} // namespace

void EffectRegistry::registerVideoEffects9()
{
    namespace k = fxkeys;
    namespace c = fxchoice;
    namespace g = effectgroups;

    add(effect(fxid::ColorSpaceTransform, tr("Color Space Transform"), g::Color,
        tr("Converts the picture from one color space and gamma to another, with tone mapping "
           "and gamut mapping for the step from HDR or wide gamut to a smaller display"),
        {
            gamutChoice(k::CmInGamut, tr("Input Color Space"), cm::Gamut::DavinciWide),
            gammaChoice(k::CmInGamma, tr("Input Gamma"), cm::Transfer::DavinciIntermediate),
            choice(k::CmTone, tr("Tone Mapping"),
                {c::ToneNone, c::ToneSimple, c::ToneLuminance, c::ToneDavinci},
                {tr("None"), tr("Simple"), tr("Luminance Mapping"), tr("DaVinci")}, c::ToneNone),
            gamutChoice(k::CmOutGamut, tr("Output Color Space"), cm::Gamut::Rec709),
            gammaChoice(k::CmOutGamma, tr("Output Gamma"), cm::Transfer::Gamma24),
            choice(k::CmGamutMap, tr("Gamut Mapping"), {c::GmNone, c::GmSaturation, c::GmCompression},
                {tr("None"), tr("Saturation Mapping"), tr("Saturation Compression")},
                c::GmNone),
            boolean(k::CmFwdOotf, tr("Apply Forward OOTF"), false),
            boolean(k::CmInvOotf, tr("Apply Inverse OOTF"), false),
            boolean(k::CmAdapt, tr("Use White Point Adaptation"), true),
            number(k::CmMaxIn, tr("Max Input Luminance"), 10, 10000, 1000, 10, kNits),
            number(k::CmMaxOut, tr("Max Output Luminance"), 10, 10000, 100, 10, kNits),
        }));

    add(effect(fxid::AcesTransform, tr("ACES Transform"), g::Color,
        tr("Moves the picture into ACES with an input transform, or out of it with an output "
           "transform (reference rendering and display)"),
        {
            choice(k::AcesVersion, tr("ACES Version"),
                {QStringLiteral("1.1"), QStringLiteral("1.2"), QStringLiteral("1.3")},
                {tr("ACES 1.1"), tr("ACES 1.2"), tr("ACES 1.3")}, QStringLiteral("1.3")),
            choice(k::AcesInput, tr("Input Transform"), acesIds(), acesLabels(),
                QStringLiteral("srgb")),
            choice(k::AcesOutput, tr("Output Transform"), acesIds(), acesLabels(),
                QStringLiteral("acescg")),
            boolean(k::AcesInvert, tr("Invert Output Transform"), false),
        }));

    add(effect(fxid::GamutMapping, tr("Gamut Mapping"), g::Color,
        tr("Brings colors that lie outside a smaller gamut back inside it, softly, and keeps the "
           "picture in its own color space"),
        {
            gamutChoice(k::CmInGamut, tr("Input Color Space"), cm::Gamut::Rec2020),
            gammaChoice(k::CmInGamma, tr("Input Gamma"), cm::Transfer::Gamma24),
            gamutChoice(k::CmTarget, tr("Target Color Space"), cm::Gamut::Rec709),
            choice(k::CmGmMode, tr("Mapping Mode"), {c::GmSaturation, c::GmCompression},
                {tr("Saturation Mapping"), tr("Saturation Compression")}, c::GmCompression),
            number(k::CmThreshold, tr("Threshold"), 0, 99, 80, 1, kPercent),
            number(k::CmLimit, tr("Compression Limit"), 101, 300, 120, 1, kPercent),
            number(k::CmPower, tr("Power"), 1, 4, 1.2, 0.05),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));

    add(effect(fxid::ChromaticAdaptation, tr("Chromatic Adaptation"), g::Color,
        tr("Moves the white point of the picture from one illuminant to another, like a white "
           "balance in XYZ"),
        {
            gamutChoice(k::CmInGamut, tr("Color Space"), cm::Gamut::Rec709),
            gammaChoice(k::CmInGamma, tr("Gamma"), cm::Transfer::Gamma24),
            methodChoice(),
            whiteChoice(k::CmSrcWhite, tr("Source White Point"), cm::WhitePoint::D65),
            number(k::CmSrcX, tr("Source White x"), 0.1, 0.7, 0.3127, 0.001),
            number(k::CmSrcY, tr("Source White y"), 0.1, 0.8, 0.3290, 0.001),
            whiteChoice(k::CmDstWhite, tr("Target White Point"), cm::WhitePoint::D50),
            number(k::CmDstX, tr("Target White x"), 0.1, 0.7, 0.3457, 0.001),
            number(k::CmDstY, tr("Target White y"), 0.1, 0.8, 0.3585, 0.001),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));

    add(effect(fxid::ColorCompressor, tr("Color Compressor"), g::Color,
        tr("Squeezes values that are too bright and colors that are too saturated into the "
           "range, softly instead of clipping them"),
        {
            gamutChoice(k::CmInGamut, tr("Color Space"), cm::Gamut::Rec709),
            gammaChoice(k::CmInGamma, tr("Gamma"), cm::Transfer::Gamma24),
            number(k::CmHiThreshold, tr("Highlight Threshold"), 0, 99, 80, 1, kPercent),
            number(k::CmHiLimit, tr("Highlight Limit"), 101, 1000, 200, 1, kPercent),
            number(k::CmSatThreshold, tr("Saturation Threshold"), 0, 99, 80, 1, kPercent),
            number(k::CmSatLimit, tr("Saturation Limit"), 101, 300, 120, 1, kPercent),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));

    {
        QVector<ParamDescriptor> params;
        params << file(k::DctlFile, tr("DCTL File"), QStringLiteral("DCTL (*.dctl)"));
        for (int i = 1; i <= 8; ++i)
            params << number(k::dctlParam(i), tr("Parameter %1 (from default)").arg(i), -1000,
                1000, 0, 0.01);
        for (int i = 1; i <= 2; ++i)
            params << color(k::dctlColor(i), tr("Color Parameter %1").arg(i),
                QColor(255, 255, 255));
        add(effect(fxid::Dctl, tr("DCTL"), g::Color,
            tr("Runs a DaVinci CTL file (.dctl) on the picture. The sliders of the file are the "
               "parameters below, in the order of the file, each as a change from the default "
               "set in the file"),
            params));
    }
}

} // namespace kk
