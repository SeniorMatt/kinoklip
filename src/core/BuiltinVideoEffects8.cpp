// SPDX-License-Identifier: GPL-3.0-or-later
// Background Removal (1.4, docs/behavior-spec.md section 78): an AI tool that cuts the subject out
// of the footage. The matte comes from a local model (src/matting); the pixels are drawn by
// render/GlBackgroundRemoval.cpp.
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

} // namespace

void EffectRegistry::registerVideoEffects8()
{
    namespace k = fxkeys;
    namespace c = fxchoice;
    EffectDescriptor d = descriptor(QLatin1String(fxid::BackgroundRemoval),
        tr("Background Removal"), EffectCategory::VideoEffect,
        tr("AI tool: cuts the subject out of the picture with a model that runs on this computer"),
        {
            choice(k::BgModel, tr("Model"), {c::BgModelAuto, c::BgModelVideo, c::BgModelImage},
                {tr("Automatic"), tr("People in video (temporal)"), tr("Any subject (per frame)")},
                c::BgModelAuto),
            choice(k::BgOutput, tr("Output"), {c::BgForeground, c::BgMatte, c::BgBackground},
                {tr("Foreground"), tr("Matte Only"), tr("Background Only")}, c::BgForeground),
            number(k::BgThreshold, tr("Threshold"), 0, 100, 50, 0.5, QStringLiteral("%")),
            number(k::BgSoftness, tr("Edge Softness"), 0, 100, 50, 0.5, QStringLiteral("%")),
            number(k::BgShrinkGrow, tr("Shrink / Grow"), -50, 50, 0, 0.5, QStringLiteral("px")),
            number(k::BgFeather, tr("Feather"), 0, 50, 0, 0.5, QStringLiteral("px")),
            choice(k::BgQuality, tr("Quality"), {c::BgPreview, c::BgFull},
                {tr("Preview (fast)"), tr("Full (slow)")}, c::BgPreview),
        });
    d.group = QLatin1String(effectgroups::Key);
    add(std::move(d));
}

} // namespace kk
