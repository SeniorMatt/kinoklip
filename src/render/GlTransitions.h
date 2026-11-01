// SPDX-License-Identifier: GPL-3.0-or-later
// GlTransitions — GPU implementations of the transitions of the EffectRegistry catalogue
// (cross_dissolve, dip_to_color, wipe, push). To add one: write its fragment shader and add a
// row to kTransitionTable in GlTransitions.cpp.
#pragma once

#include "core/Animation.h"
#include "render/GlKit.h"

#include <QSize>

#include <memory>
#include <span>

namespace kk::render {

/// Input of one transition draw. `a` (outgoing) and `b` (incoming) are output-size textures
/// holding the two clips already transformed and composited (premultiplied); a missing side
/// (fade from/to transparent at a track end) is GlKit::transparentTexture().
struct TransitionInput
{
    GLuint a = 0;
    GLuint b = 0;
    double progress = 0.0; ///< 0 = all A, 1 = all B (TransitionSpan::progress())
    const ParamSet* params = nullptr;
    int64_t localFrame = 0; ///< frame inside the transition window (for animated params)
    QSize size; ///< output size in pixels
};

/// One transition: a single full-frame pass combining A and B into the bound target. The
/// caller sets "over" blending, so the result composites onto the tracks below.
class GlTransition
{
public:
    virtual ~GlTransition() = default;
    virtual bool initialize(GlKit& kit, QString* why) = 0;
    virtual void destroy(GlKit& kit) = 0;
    virtual void draw(GlKit& kit, const TransitionInput& in) = 0;
};

/// Registration row: descriptor id (EffectRegistry, category Transition) → factory.
struct GlTransitionFactory
{
    const char* id;
    std::unique_ptr<GlTransition> (*create)();
};

/// All built-in GPU transitions (the single registration point).
[[nodiscard]] std::span<const GlTransitionFactory> builtinGlTransitions();

/// Unit vector (x right, y down) of a direction choice "left"/"right"/"up"/"down": the way the
/// incoming picture's edge travels. Unknown → left.
[[nodiscard]] QPointF directionVector(const QString& direction);

} // namespace kk::render
