// SPDX-License-Identifier: GPL-3.0-or-later
// GlTransitions — GPU implementations of the transitions of the EffectRegistry catalogue
// (cross_dissolve, dip_to_color, wipe, push, and the folders Dissolve, Iris, Shape, Wipe, Motion).
// To add one: write its fragment shader (GlShaderTransition.h has the class that runs one) and
// register it in the append...Transitions() function of its family.
#pragma once

#include "core/Animation.h"
#include "render/GlKit.h"

#include <QSize>

#include <functional>
#include <memory>
#include <vector>

namespace kk::render {

class GlRetime;

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
    /// The target the transition draws into (already bound when draw() starts); a transition
    /// that renders into other targets first binds it again before its final pass.
    const RenderTarget* target = nullptr;
    GlRetime* retime = nullptr; ///< optical-flow interpolator, null when it failed to build
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
    QString id;
    std::function<std::unique_ptr<GlTransition>()> create;
};

/// All built-in GPU transitions (the single registration point, built on first use).
[[nodiscard]] const std::vector<GlTransitionFactory>& builtinGlTransitions();

/// The families, each in its own file; builtinGlTransitions() collects them.
void appendDissolveTransitions(
    std::vector<GlTransitionFactory>& out); ///< GlDissolveTransitions.cpp
void appendShapeTransitions(std::vector<GlTransitionFactory>& out); ///< GlShapeTransitions.cpp
void appendMotionTransitions(std::vector<GlTransitionFactory>& out); ///< GlMotionTransitions.cpp
void appendFusionTransitions(std::vector<GlTransitionFactory>& out); ///< GlFusionTransitions.cpp

/// Unit vector (x right, y down) of a direction choice "left"/"right"/"up"/"down": the way the
/// incoming picture's edge travels. Unknown → left.
[[nodiscard]] QPointF directionVector(const QString& direction);

} // namespace kk::render
