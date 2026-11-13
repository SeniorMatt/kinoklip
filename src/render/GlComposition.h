// SPDX-License-Identifier: GPL-3.0-or-later
// GlComposition — GPU evaluation of a Compositing page node graph (core/Composition.h) for
// one comp frame: a pull from MediaOut (or any node, for the page's viewers), each node one
// or two passes over premultiplied RGBA16F targets at comp resolution.
#pragma once

#include "core/Composition.h"
#include "render/CompMath.h"
#include "render/GlEffects.h"
#include "render/GlGenerators.h"
#include "render/GlKit.h"
#include "render/GlMasks.h"
#include "render/TitleRenderer.h"

#include <QHash>
#include <QSet>
#include <QSize>
#include <QSizeF>

#include <functional>
#include <memory>
#include <vector>

namespace kk::render {

/// What a composition reads from the renderer that owns it (GlRenderer).
class CompSources
{
public:
    virtual ~CompSources() = default;
    /// Draws the picture of MediaIn @p node at comp frame @p compFrame into @p target (cleared,
    /// comp size): the source fitted into the frame like a clip at zoom 1. False if there is
    /// nothing to draw (offline media, not decoded). May bind other targets meanwhile.
    virtual bool drawMediaIn(const CompNode& node, int64_t compFrame, const RenderTarget& target)
        = 0;
    /// A cached texture of @p style (TitleRenderer); 0 on failure. @p rect: where the image
    /// lies in the canvas (output px).
    virtual GLuint titleTexture(const TitleStyle& style, QRectF* rect) = 0;
};

/// One evaluation request.
struct CompEval
{
    const Composition* comp = nullptr;
    int64_t compFrame = 0; ///< clip-local frame (keyframes)
    QSizeF frame; ///< timeline size (node units)
    QSize size; ///< target pixels (frame × scale)
    double scale = 1.0; ///< target pixels per timeline pixel
    double fps = 25.0;
    CompSources* sources = nullptr;
};

/// Evaluator of compositions (render thread only, like GlKit).
///
/// Approach: compops::evaluationOrder() gives the nodes the pulled node depends on in
/// dependency order; every node is evaluated once per call (the per-frame cache), and its
/// target is released as soon as the last node reading it has run (reference counts), so a
/// chain of ten nodes needs only two or three live targets. Disabled nodes, identity effects
/// and MediaOut alias their main input (no pass). Mask inputs multiply the node's effect:
/// result = mix(input, effect, mask alpha), done in the node's own pass where it has one
/// (Merge) or in one extra mix pass. See src/render/readme.md, "Compositions".
class GlComposition
{
public:
    /// Compiles the programs. False with a reason.
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);
    /// The renderer's effect lookup (descriptor id -> effect, nullptr when there is none; the
    /// renderer builds an effect on its first use), masks and generators (owned by the
    /// renderer, alive while this is used).
    void setShared(
        std::function<GlEffect*(const QString&)> effects, GlMasks* masks, GlGenerators* generators)
    {
        m_effects = std::move(effects);
        m_masks = masks;
        m_generators = generators;
    }

    /// The image of node @p nodeId at the request's frame, as a new target of `ev.size`
    /// (premultiplied RGBA16F; transparent where the node draws nothing). An unknown node
    /// gives a transparent target.
    [[nodiscard]] PooledTarget evaluate(GlKit& kit, const CompEval& ev, quint64 nodeId);

    /// The Merge node's blend of two full-frame pictures: @p layer (premultiplied, its
    /// opacity already applied) over @p background with blend mode @p mode (blendmodes::
    /// index), drawn into the bound target of the same size (blending off). The timeline's
    /// composite modes (Inspector > Composite) use it, so both pages blend alike.
    void blendLayers(GlKit& kit, GLuint background, GLuint layer, int mode);

private:
    /// Result of one node during an evaluation.
    struct Slot
    {
        quint64 id = 0;
        PooledTarget own; ///< the node's own target (empty: aliased or transparent)
        int owner = -1; ///< slot whose target is this node's image (-1 = transparent)
        int refs = 0; ///< remaining reads of `own` (counted on the owner slot)
    };

    void evalNode(GlKit& kit, const CompEval& ev, int index);
    /// Index into m_slots of node @p id, or -1.
    [[nodiscard]] int slotOf(quint64 id) const;
    /// The image slot of input @p port of @p node (-1 = transparent / not connected).
    [[nodiscard]] int inputOwner(const CompNode& node, const QString& port) const;
    [[nodiscard]] GLuint textureOf(GlKit& kit, int owner) const;
    /// Drops one read of @p owner's target (released when no read is left).
    void release(int owner);
    /// Gives slot @p index the image of slot @p owner (pass-through).
    void alias(int index, int owner);

    PooledTarget newTarget(GlKit& kit, const CompEval& ev, bool clear);
    /// mix(a, b, amount · mask alpha) into a new target (a / b / mask may be 0 = transparent
    /// or, for the mask, "everywhere").
    PooledTarget mixPass(
        GlKit& kit, const CompEval& ev, GLuint a, GLuint b, GLuint mask, float amount);

    /// The Common Controls (compmath::applyCommon()) of the result @p result of a node whose
    /// main input is @p in (0 = transparent); @p mask 0 = no Effect Mask.
    PooledTarget commonPassOf(GlKit& kit, const CompEval& ev, const compmath::CommonParams& c,
        GLuint in, GLuint result, GLuint mask);

    /// @p texture drawn through @p inputToOutput (timeline px) by the per-pixel resampler.
    PooledTarget resamplePass(GlKit& kit, const CompEval& ev, GLuint texture,
        const QTransform& inputToOutput, const compmath::Resampling& r, bool* invertible);
    PooledTarget merge(GlKit& kit, const CompEval& ev, const CompNode& n, int bg, int fg, int mask);
    PooledTarget transformNode(GlKit& kit, const CompEval& ev, const CompNode& n, int input);
    PooledTarget rectNode(GlKit& kit, const CompEval& ev, const CompNode& n, int input);
    PooledTarget background(GlKit& kit, const CompEval& ev, const CompNode& n);
    PooledTarget text(GlKit& kit, const CompEval& ev, const CompNode& n);
    PooledTarget noise(GlKit& kit, const CompEval& ev, const CompNode& n);
    PooledTarget maskNode(GlKit& kit, const CompEval& ev, const CompNode& n, int input);
    PooledTarget colorNode(GlKit& kit, const CompEval& ev, const CompNode& n, int input);
    PooledTarget effectNode(GlKit& kit, const CompEval& ev, const CompNode& n, int input);

    std::function<GlEffect*(const QString&)> m_effects;
    GlMasks* m_masks = nullptr;
    GlGenerators* m_generators = nullptr;

    GlProgram m_merge;
    GLint m_mergeFgMatrix = -1, m_mergeMode = -1, m_mergeOpacity = -1, m_mergeHasFg = -1,
          m_mergeHasMask = -1;
    GlProgram m_mix;
    GLint m_mixAmount = -1, m_mixHasMask = -1;
    GlProgram m_common;
    GLint m_commonHasMask = -1, m_commonBlend = -1, m_commonChannels = -1, m_commonInverted = -1,
          m_commonMultiply = -1;
    GlProgram m_corrector;
    GLint m_ccLevels = -1, m_ccLgg = -1, m_ccSat = -1;
    GlProgram m_channels;
    GLint m_chSources = -1;
    GlProgram m_lumaKey, m_deltaKey, m_paintMask;
    GLint m_lkParams = -1, m_lkInvert = -1, m_dkBg = -1, m_dkTune = -1, m_dkClean = -1,
          m_dkInvert = -1, m_pmMode = -1;
    GlProgram m_resample;
    GLint m_resampleInverse = -1, m_resampleEdges = -1, m_resampleFilter = -1;
    GLint m_mergeOp = -1, m_mergeAlphaGain = -1, m_mergeSubAdd = -1, m_mergeBurnIn = -1;
    GlProgram m_warp;
    GLint m_warpInverse = -1;
    GlProgram m_noise;
    GLint m_nSize = -1, m_nCentre = -1, m_nRot = -1, m_nScale = -1, m_nDetail = -1, m_nShape = -1,
          m_nTime = -1, m_nSeed = -1, m_nColor1 = -1, m_nColor2 = -1;

    // Per-evaluation scratch (capacity reused): one slot list per re-entry depth (a MediaIn
    // showing a nested timeline may evaluate another composition while this one runs).
    static constexpr int kMaxDepth = 18;
    std::vector<std::vector<Slot>> m_stack;
    int m_depth = 0; ///< current list (0 = unused, 1 = outermost evaluation)
    ClipData m_maskClip; ///< carries one MaskData for GlMasks::apply()
    QSet<QString> m_warned; ///< node types without a GPU implementation, reported once
};

} // namespace kk::render
