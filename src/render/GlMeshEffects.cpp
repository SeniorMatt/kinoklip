// SPDX-License-Identifier: GPL-3.0-or-later
// GlMeshEffects — GPU table rows of the tracking effects (spec section 70). Match Move draws
// nothing itself: it moves the clip through render::clipTransform(), so its GPU effect is an
// always-identity stand-in that keeps the renderer from warning about a missing implementation.
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"

namespace kk::render {

namespace {

using namespace fx;

class MovesTheClip final : public GlEffect
{
public:
    bool initialize(GlKit&, QString*) override { return true; }
    void destroy(GlKit&) override { }
    bool isIdentity(const ParamSet&, int64_t) const override { return true; }
    PooledTarget apply(GlKit&, const EffectInput&) override { return {}; }
};

const GlEffectFactory kMeshTable[] = {
    {fxid::MatchMove, &make<MovesTheClip>},
};

} // namespace

std::span<const GlEffectFactory> meshGlEffects()
{
    return kMeshTable;
}

} // namespace kk::render
