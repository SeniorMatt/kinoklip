// SPDX-License-Identifier: GPL-3.0-or-later
// GlOfxEffect — an OpenFX image effect (plugins/OfxRegistry.h, ofx/OfxHost.h) as a GlEffect
// (internal to src/render): the layer is read back as premultiplied float RGBA, the plugin renders
// it on the CPU (on the render thread; the plugin may use more threads through the OFX
// multi-thread suite) and the result is uploaded into a new layer target. Spec 158.
#pragma once

#include "render/GlEffects.h"
#include "render/GlTransitions.h"

#include <QString>

#include <memory>

namespace kk::render {

/// The effect for descriptor id @p fullId ("ofx:<identifier>"), or null when no plugin provides it.
[[nodiscard]] std::unique_ptr<GlEffect> makeOfxEffect(const QString& fullId);
/// A transition made of an OpenFX plugin of the Transition context (SourceFrom, SourceTo and a
/// "Transition" progress parameter); null for an unknown plugin or one that is not a transition.
[[nodiscard]] std::unique_ptr<GlTransition> makeOfxTransition(const QString& fullId);
/// True for the ids of OpenFX effects.
[[nodiscard]] bool isOfxId(const QString& id);

} // namespace kk::render
