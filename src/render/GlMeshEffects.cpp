// SPDX-License-Identifier: GPL-3.0-or-later
// GlMeshEffects — the GPU rows of the tracking effects of spec section 70: Surface Tracker in Warp
// mode (the grid is the tracked mesh; Warper joins it with a grid the user moves) and the stand-in
// of Match Move. Both are one pass with the same shader: a displacement per grid node
// (Catmull-Rom interpolated over the grid, so a node moves exactly the picture that sits on it
// and nodes far away move nothing), and every output pixel reads the picture where the field
// says it came from (a few fixed-point steps invert the field). Standard methods.
#include "core/EffectTrack.h"
#include "core/SurfaceTracker.h"
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

/// Most grid points per side the shader holds.
constexpr int kMaxSide = 9;

/// A grid of displacements in content units (0..1 across the content).
struct MeshGrid
{
    int cols = 0, rows = 0;
    float rest[4] = {0, 0, 1, 1}; ///< the rest rectangle of the grid, content units
    std::vector<float> d; ///< 2 floats per node, row by row (row 0 at the top)
    [[nodiscard]] bool isIdentity() const
    {
        return std::all_of(d.begin(), d.end(), [](float v) { return std::abs(v) < 1e-7f; });
    }
};

const char* const kMeshShader = R"(
uniform sampler2D uTex;
uniform vec4 uContent;  // content rectangle in uv: x0, y0, x1, y1
uniform int uCols;
uniform int uRows;
uniform vec4 uRest;     // rest rectangle of the grid in content units
uniform vec2 uD[81];    // displacement per node in content units
vec2 node(int c, int r)
{
    c = clamp(c, 0, uCols - 1);
    r = clamp(r, 0, uRows - 1);
    return uD[r * uCols + c];
}
vec4 catmullRom(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0,
        -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
vec2 field(vec2 t)
{
    vec2 top = vec2(uCols - 1, uRows - 1);
    vec2 g = clamp((t - uRest.xy) / (uRest.zw - uRest.xy) * top, vec2(0.0), top);
    ivec2 i = ivec2(floor(g));
    vec2 f = g - vec2(i);
    vec4 wx = catmullRom(f.x);
    vec4 wy = catmullRom(f.y);
    vec2 sum = vec2(0.0);
    for (int j = -1; j <= 2; ++j) {
        vec2 row = vec2(0.0);
        for (int c = -1; c <= 2; ++c)
            row += wx[c + 1] * node(i.x + c, i.y + j);
        sum += wy[j + 1] * row;
    }
    return sum;
}
void main()
{
    vec2 size = uContent.zw - uContent.xy;
    vec2 t = (vUv - uContent.xy) / size;
    vec2 s = t;
    for (int n = 0; n < 5; ++n)
        s = t - field(s);
    vec2 uv = uContent.xy + s * size;
    bool outside = any(lessThan(uv, uContent.xy)) || any(greaterThan(uv, uContent.zw));
    fragColor = outside ? vec4(0.0) : texture(uTex, uv);
}
)";

/// Common part: the shader and the upload of a MeshGrid.
class MeshEffect : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, name(), kMeshShader, why))
            return false;
        m_content = uniform(kit, "uContent");
        m_cols = uniform(kit, "uCols");
        m_rows = uniform(kit, "uRows");
        m_rest = uniform(kit, "uRest");
        m_d = uniform(kit, "uD");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override { return identity(p, f); }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        MeshGrid grid;
        if (!makeGrid(in, &grid) || grid.cols < 2 || grid.rows < 2 || grid.isIdentity())
            return {};
        const LayerGeometry g = layerGeometry(in);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform4f(m_content, g.x0, g.y0, g.x1, g.y1);
        gl->glUniform1i(m_cols, grid.cols);
        gl->glUniform1i(m_rows, grid.rows);
        gl->glUniform4f(m_rest, grid.rest[0], grid.rest[1], grid.rest[2], grid.rest[3]);
        gl->glUniform2fv(m_d, grid.cols * grid.rows, grid.d.data());
        draw(kit, *in.layer);
        return out;
    }

protected:
    virtual const char* name() const = 0;
    virtual bool identity(const ParamSet& p, int64_t f) const = 0;
    /// The grid for this frame; false = leave the picture alone.
    virtual bool makeGrid(const EffectInput& in, MeshGrid* grid) const = 0;

    /// Content size in layer pixels.
    static QSizeF contentPx(const EffectInput& in)
    {
        return QSizeF(in.layer->size.width() - 2 * in.paddingPx,
            in.layer->size.height() - 2 * in.paddingPx);
    }

private:
    GLint m_content = -1, m_cols = -1, m_rows = -1, m_rest = -1, m_d = -1;
};

// ---- surface_tracker (warp mode) ----

class SurfaceWarp final : public MeshEffect
{
protected:
    const char* name() const override { return "surface_warp"; }
    bool identity(const ParamSet& p, int64_t) const override
    {
        return p.staticValue(k::SurfaceMode, fxchoice::SurfacePin).toString()
            != fxchoice::SurfaceWarp;
    }
    bool makeGrid(const EffectInput& in, MeshGrid* grid) const override
    {
        const ParamSet& p = *in.params;
        if (p.staticValue(k::SurfaceMode, fxchoice::SurfacePin).toString() != fxchoice::SurfaceWarp)
            return false;
        const auto table = efftrack::decode(p.staticValue(k::TrackData).toString());
        if (!table || table->signature() != surface::signature(p))
            return false;
        surface::Mesh mesh;
        if (!surface::meshDisplacement(*table, in.sourceSeconds, &mesh))
            return false;
        const double strength = std::clamp(p.value(k::Strength, in.localFrame, 100.0), 0.0, 200.0) / 100.0;
        grid->cols = mesh.cols;
        grid->rows = mesh.rows;
        std::copy(std::begin(mesh.rest), std::end(mesh.rest), grid->rest);
        grid->d.resize(mesh.d.size());
        for (size_t i = 0; i < mesh.d.size(); ++i)
            grid->d[i] = float(mesh.d[i] * strength);
        return true;
    }
};

/// Match Move draws nothing itself: it moves the clip through render::clipTransform(), so its GPU
/// effect is an always-identity stand-in (keeps the renderer from warning about a missing one).
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
    {fxid::SurfaceTracker, &make<SurfaceWarp>},
};

} // namespace

std::span<const GlEffectFactory> meshGlEffects()
{
    return kMeshTable;
}

} // namespace kk::render
