// SPDX-License-Identifier: GPL-3.0-or-later
// ColorManagementFx — the parameters of the colour management effects (Color Space Transform,
// ACES Transform, Gamut Mapping, Chromatic Adaptation, Color Compressor; spec 205) resolved
// into cm::CstParams, and their CPU path. The GPU twin is GlColorManagement.cpp.
#pragma once

#include "core/ColorSpaces.h"
#include "render/EffectMath.h"

namespace kk::render::fxmath {

/// True for the five effect types above.
[[nodiscard]] bool isColorManagementEffect(const QString& type);
/// The resolved pipeline of effect @p type with @p p at @p localFrame.
[[nodiscard]] cm::CstParams colorManagementParams(
    const QString& type, const ParamSet& p, int64_t localFrame);
/// True when the effect leaves the picture alone.
[[nodiscard]] bool colorManagementIdentity(
    const QString& type, const ParamSet& p, int64_t localFrame);
/// The CPU path on an ARGB32_Premultiplied @p image. @return false for other types.
bool applyColorManagementEffect(
    const QString& type, const ParamSet& p, int64_t localFrame, QImage& image);

} // namespace kk::render::fxmath
