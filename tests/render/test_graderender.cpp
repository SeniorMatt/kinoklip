// SPDX-License-Identifier: GPL-3.0-or-later
// Color page rendering (docs/color-page.md, spec 44a): the GPU grade pass against the CPU
// reference (render/GradeEval) for every node feature, the CPU fallback backend, the place of
// the clip grade in the clip pipeline (after the Edit page effects, moving with the clip), the
// timeline grade over the composite, "bypass all grades", and the render cache keys. Small
// frames (64 x 36): the machine runs llvmpipe. GPU cases skip without OpenGL.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/GradeMath.h"
#include "core/VideoEffectKeys.h"
#include "render/Compositor.h"
#include "render/CubeLut.h"
#include "render/GradeEval.h"
#include "render/GradeLut.h"
#include "render/RenderCacheKeys.h"
#include "render/TimelineEval.h"

#include <QFile>
#include <QTemporaryDir>

#include <cmath>

using namespace kk;
using namespace kk::test;
using namespace kk::render;
namespace go = kk::gradeops;
namespace gk = kk::gradekeys;
namespace wk = kk::gradewindowkeys;

namespace {

constexpr int kW = 64;
constexpr int kH = 36;

/// Largest per-channel difference of two images of the same size (alpha included).
int maxDiff(const QImage& a, const QImage& b, QPoint* where = nullptr)
{
    if (a.size() != b.size())
        return 1000;
    const QImage x = a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage y = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int worst = 0;
    for (int j = 0; j < x.height(); ++j) {
        for (int i = 0; i < x.width(); ++i) {
            const QRgb p = x.pixel(i, j), q = y.pixel(i, j);
            const int d = std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
            if (d > worst) {
                worst = d;
                if (where)
                    *where = QPoint(i, j);
            }
        }
    }
    return worst;
}

/// One corrector with @p values.
ClipGrade primariesGrade(std::initializer_list<std::pair<const QString, double>> values)
{
    ClipGrade g;
    const quint64 n = go::initialize(g);
    for (const auto& [k, v] : values)
        g.node(n)->params.setBase(k, v);
    return g;
}

QByteArray cube3d(int n)
{
    // A look: channels swapped and bent (non-linear, so the interpolations differ).
    QByteArray text = "TITLE \"look\"\nLUT_3D_SIZE " + QByteArray::number(n) + "\n";
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                const double R = double(r) / (n - 1), G = double(g) / (n - 1),
                             B = double(b) / (n - 1);
                text += QByteArray::number(std::pow(G, 0.8), 'f', 6) + ' '
                    + QByteArray::number(B * B, 'f', 6) + ' '
                    + QByteArray::number(0.2 + 0.6 * R, 'f', 6) + '\n';
            }
    return text;
}

QByteArray cube1d(int n)
{
    QByteArray text = "LUT_1D_SIZE " + QByteArray::number(n) + "\n";
    for (int i = 0; i < n; ++i) {
        const double v = double(i) / (n - 1);
        text += QByteArray::number(1.0 - v, 'f', 6) + ' ' + QByteArray::number(v * v, 'f', 6) + ' '
            + QByteArray::number(std::sqrt(v), 'f', 6) + '\n';
    }
    return text;
}

} // namespace

class TestGradeRender : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    QTemporaryDir m_dir;
    QString m_lut3d, m_lut1d;

    bool gpuReady() const { return m_gpu && m_gpu->isGpu(); }

    /// The test timeline: the ramp picture on V1 (fills the frame exactly) with @p clipGrade,
    /// the timeline graded with @p timelineGrade.
    TimelineData timelineWith(const ClipGrade& clipGrade, const ClipGrade& timelineGrade = {})
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = mediaClip(tl, QStringLiteral("ramp"), 0, 10);
        clip.grade = clipGrade;
        addClip(tl, TrackType::Video, 0, std::move(clip));
        tl.grade = timelineGrade;
        return tl;
    }

    QImage render(Compositor& c, const TimelineData& tl, bool bypass = false, int64_t frame = 0)
    {
        RenderRequest rq;
        rq.timeline = snap(tl);
        rq.frame = frame;
        rq.outputs = OutputImage;
        rq.bypassGrades = bypass;
        const RenderedFramePtr f = c.renderAsync(std::move(rq)).get();
        return f && f->ok ? f->image.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                          : QImage();
    }

    /// The grades every renderer test runs: name, grade.
    QVector<std::pair<QString, ClipGrade>> cases()
    {
        QVector<std::pair<QString, ClipGrade>> out;
        out.append({QStringLiteral("wheels"),
            primariesGrade({{gk::LiftB, 0.1}, {gk::GammaR, 0.2}, {gk::GainG, 1.3},
                {gk::OffsetY, -0.05}, {gk::GainY, 1.1}, {gk::LumMix, 0.5}})});
        out.append({QStringLiteral("contrast_sat_hue_temp"),
            primariesGrade({{gk::Contrast, 1.4}, {gk::Pivot, 0.4}, {gk::Saturation, 1.5},
                {gk::Hue, 40.0}, {gk::Temperature, 0.3}, {gk::Tint, -0.2}})});
        {
            ClipGrade g = primariesGrade({});
            GradeNode& n = g.nodes[0];
            n.curves[GradeCurve::Luma] = {{0.25, 0.15}, {0.75, 0.85}};
            n.curves[GradeCurve::Green] = {{0.5, 0.6}};
            n.curves[GradeCurve::HueHue] = {{0.0, 0.5}, {0.3, 0.6}, {0.7, 0.45}};
            n.curves[GradeCurve::HueSat] = {{0.1, 0.8}, {0.6, 0.3}};
            n.curves[GradeCurve::HueLum] = {{0.2, 0.6}, {0.8, 0.45}};
            n.curves[GradeCurve::LumSat] = {{0.2, 0.3}, {0.8, 0.7}};
            n.curves[GradeCurve::SatSat] = {{0.3, 0.6}, {0.9, 0.4}};
            out.append({QStringLiteral("curves"), g});
        }
        {
            ClipGrade g = primariesGrade({{gk::Saturation, 0.0}, {gk::GainY, 1.2}});
            GradeQualifier& q = g.nodes[0].qualifier;
            q.enabled = true;
            q.hueCenter = 0.0;
            q.hueWidth = 0.2;
            q.hueSoft = 0.1;
            q.satLow = 0.1;
            q.lumLow = 0.1;
            q.lumHigh = 0.9;
            out.append({QStringLiteral("qualifier"), g});
            q.invert = true;
            out.append({QStringLiteral("qualifier_inverted"), g});
            // Qualifier Matte Finesse (spec 108): every control alone, then all together.
            auto finesse = [&](const char* name, auto&& set) {
                ClipGrade f = g;
                f.nodes[0].qualifier.invert = false;
                f.nodes[0].qualifier.hueWidth = 0.4;
                set(f.nodes[0].qualifier);
                out.append({QStringLiteral("matte_") + QLatin1String(name), f});
            };
            finesse("clean_black", [](GradeQualifier& q) { q.cleanBlack = 1.0; });
            finesse("clean_white", [](GradeQualifier& q) { q.cleanWhite = 1.0; });
            finesse("shrink", [](GradeQualifier& q) { q.shrinkGrow = -1.0; });
            finesse("grow", [](GradeQualifier& q) { q.shrinkGrow = 1.0; });
            finesse("denoise", [](GradeQualifier& q) { q.denoise = 1.0; });
            finesse("blur", [](GradeQualifier& q) { q.blurRadius = 1.0; });
            finesse("blur_in", [](GradeQualifier& q) { q.blurRadius = 1.0; q.inOutRatio = 0.8; });
            finesse("blur_out", [](GradeQualifier& q) { q.blurRadius = 1.0; q.inOutRatio = -0.8; });
            finesse("all", [](GradeQualifier& q) {
                q.cleanBlack = 1.0;
                q.cleanWhite = 1.0;
                q.denoise = 0.6;
                q.shrinkGrow = 0.5;
                q.blurRadius = 0.8;
                q.inOutRatio = 0.3;
            });
        }
        {
            // The RGB, Luma and 3D qualifier modes (spec 108).
            ClipGrade g = primariesGrade({{gk::Saturation, 0.0}, {gk::GainY, 1.2}});
            GradeQualifier& q = g.nodes[0].qualifier;
            q.enabled = true;
            q.mode = QualifierMode::Rgb;
            q.redLow = 0.5;
            q.blueHigh = 0.7;
            q.greenSoft = 0.2;
            out.append({QStringLiteral("qualifier_rgb"), g});
            q.useGreen = false;
            q.blurRadius = 0.5;
            out.append({QStringLiteral("qualifier_rgb_blurred"), g});
            q = GradeQualifier {};
            q.enabled = true;
            q.mode = QualifierMode::Luma;
            q.lumLow = 0.35;
            q.lumHigh = 0.8;
            out.append({QStringLiteral("qualifier_luma"), g});
            q = GradeQualifier {};
            q.enabled = true;
            q.mode = QualifierMode::ThreeD;
            q.colors = {{0.9f, 0.3f, 0.2f, false}, {0.2f, 0.4f, 0.9f, false}, {0.9f, 0.5f, 0.5f, true}};
            q.colorRange = 0.25;
            q.colorSoft = 0.2;
            out.append({QStringLiteral("qualifier_3d"), g});
            q.denoise = 0.4;
            q.shrinkGrow = 0.5;
            out.append({QStringLiteral("qualifier_3d_finesse"), g});
        }
        {
            ClipGrade g = primariesGrade({{gk::GainR, 0.3}, {gk::GainB, 1.6}});
            const quint64 n = g.nodes[0].id;
            const quint64 circle = go::addWindow(g, n, GradeWindowShape::Circle);
            g.window(circle)->params.setBase(wk::CenterX, 0.3);
            g.window(circle)->params.setBase(wk::SoftOut, 0.15);
            const quint64 rect = go::addWindow(g, n, GradeWindowShape::Linear);
            g.window(rect)->params.setBase(wk::Rotation, 30.0);
            g.window(rect)->params.setBase(wk::CenterX, 0.75);
            g.window(rect)->params.setBase(wk::SoftIn, 0.05);
            out.append({QStringLiteral("circle_linear"), g});
            ClipGrade p = primariesGrade({{gk::Saturation, 0.2}});
            const quint64 m = p.nodes[0].id;
            const quint64 poly = go::addWindow(p, m, GradeWindowShape::Polygon);
            p.window(poly)->points = {{QPointF(-0.5, -0.4)}, {QPointF(0.4, -0.5)},
                {QPointF(0.1, 0.0)}, {QPointF(0.5, 0.5)}, {QPointF(-0.3, 0.3)}};
            p.window(poly)->params.setBase(wk::Width, 0.8);
            p.window(poly)->params.setBase(wk::Height, 0.9);
            p.window(poly)->params.setBase(wk::SoftOut, 0.08);
            const quint64 grad = go::addWindow(p, m, GradeWindowShape::Gradient);
            p.window(grad)->params.setBase(wk::Rotation, -60.0);
            p.window(grad)->invert = true;
            p.window(grad)->params.setBase(wk::Opacity, 0.7);
            out.append({QStringLiteral("polygon_gradient"), p});
            // Curve window (spec 111): smooth and corner points, softness both sides.
            ClipGrade c = primariesGrade({{gk::Saturation, 0.2}});
            const quint64 cm = c.nodes[0].id;
            const quint64 curve = go::addWindow(c, cm, GradeWindowShape::Curve);
            c.window(curve)->points = {
                {QPointF(-0.4, -0.3), QPointF(-0.2, 0.1), QPointF(0.25, -0.2), true},
                {QPointF(0.45, -0.1), QPointF(-0.1, -0.2), QPointF(0.0, 0.3), true},
                {QPointF(0.1, 0.45)}, {QPointF(-0.45, 0.4), QPointF(0.2, 0.15), QPointF(), false}};
            c.window(curve)->params.setBase(wk::Width, 0.8);
            c.window(curve)->params.setBase(wk::Height, 0.8);
            c.window(curve)->params.setBase(wk::SoftIn, 0.03);
            c.window(curve)->params.setBase(wk::SoftOut, 0.06);
            out.append({QStringLiteral("curve_window"), c});
        }
        {
            // Blur palette (spec 106): the three modes, per-channel radii, a window and its
            // inverse, after the colour corrections.
            out.append({QStringLiteral("blur_radius"),
                primariesGrade({{gk::BlurRadiusR, 1.0}, {gk::BlurRadiusG, 1.0},
                    {gk::BlurRadiusB, 1.0}, {gk::BlurScaling, 1.0}})});
            ClipGrade g = primariesGrade({{gk::BlurRadiusR, 1.0}, {gk::BlurRadiusG, 0.8},
                {gk::BlurRadiusB, 0.6}, {gk::BlurScaling, 1.0}, {gk::BlurRatio, 0.3},
                {gk::Saturation, 1.4}});
            const quint64 n = g.nodes[0].id;
            const quint64 circle = go::addWindow(g, n, GradeWindowShape::Circle);
            g.window(circle)->params.setBase(wk::Width, 0.4);
            g.window(circle)->params.setBase(wk::SoftOut, 0.1);
            out.append({QStringLiteral("blur_circle_window"), g});
            g.window(circle)->invert = true;
            out.append({QStringLiteral("blur_inverted_window"), g});
            out.append({QStringLiteral("sharpen_mist"),
                primariesGrade({{gk::SharpRadiusR, 1.0}, {gk::SharpRadiusG, 1.0},
                    {gk::SharpRadiusB, 1.0}, {gk::SharpLevel, 1.0}, {gk::SharpScaling, 1.0},
                    {gk::SharpCoring, 0.2}, {gk::MistRadius, 0.8}, {gk::MistMix, 0.7},
                    {gk::MistScaling, 1.0}})});
        }
        {
            // Tracker palette (spec 107): a tracked circle window, moved, grown and turned, on
            // the GPU path and the CPU reference alike (both read the same evaluated window).
            ClipGrade g = primariesGrade({{gk::GainR, 1.6}, {gk::GainG, 1.6}, {gk::GainB, 1.6}, {gk::BlurRadiusR, 1.0},
                {gk::BlurRadiusG, 1.0}, {gk::BlurRadiusB, 1.0}, {gk::BlurScaling, 1.0}});
            const quint64 circle = go::addWindow(g, g.nodes[0].id, GradeWindowShape::Linear);
            g.window(circle)->params.setBase(wk::CenterX, 0.4);
            g.window(circle)->params.setBase(wk::Width, 0.3);
            g.window(circle)->params.setBase(wk::Height, 0.4);
            g.window(circle)->params.setBase(wk::SoftOut, 0.05);
            GradeTrack& t = g.nodes[0].track;
            t.pivot = QPointF(0.4, 0.5);
            t.zoom = t.rotate = true;
            t.samples = {{0.2, 0.1, 1.3, 1.3, 25.0}};
            out.append({QStringLiteral("tracked_window"), g});
        }
        {
            // RGB Mixer (spec 109): a cross mix, Monochrome, Preserve Luminance (both modes),
            // after the primaries and before the curves.
            out.append({QStringLiteral("mixer_matrix"),
                primariesGrade({{gk::MixerRR, 0.4}, {gk::MixerRG, 0.7}, {gk::MixerRB, -0.1},
                    {gk::MixerGR, 0.2}, {gk::MixerBB, 1.3}, {gk::MixerBR, 0.5},
                    {gk::Saturation, 1.2}})});
            out.append({QStringLiteral("mixer_mono"),
                primariesGrade({{gk::MixerMono, 1.0}, {gk::MixerMonoR, 0.6},
                    {gk::MixerMonoG, 0.7}, {gk::MixerMonoB, -0.2}})});
            out.append({QStringLiteral("mixer_mono_preserve"),
                primariesGrade({{gk::MixerMono, 1.0}, {gk::MixerPreserve, 1.0},
                    {gk::MixerMonoR, 0.6}, {gk::MixerMonoG, 0.7}, {gk::MixerMonoB, -0.2}})});
            out.append({QStringLiteral("mixer_preserve"),
                primariesGrade({{gk::MixerPreserve, 1.0}, {gk::MixerRG, 0.9}, {gk::MixerGG, 0.3},
                    {gk::MixerBR, 0.8}, {gk::Contrast, 1.2}})});
        }
        {
            // AgX (spec 112): the default Rec. 709 input, a camera log, each look, sRGB output,
            // an exposure, and after other corrections.
            out.append({QStringLiteral("agx_base"), primariesGrade({{gk::AgxOn, 1.0}})});
            out.append({QStringLiteral("agx_punchy"),
                primariesGrade({{gk::AgxOn, 1.0}, {gk::AgxLook, 1.0}, {gk::AgxExposure, 0.5}})});
            out.append({QStringLiteral("agx_golden_srgb"),
                primariesGrade({{gk::AgxOn, 1.0}, {gk::AgxLook, 2.0}, {gk::AgxOutput, 1.0},
                    {gk::AgxInput, 1.0}})});
            out.append({QStringLiteral("agx_greyscale"),
                primariesGrade({{gk::AgxOn, 1.0}, {gk::AgxLook, 3.0}})});
            out.append({QStringLiteral("agx_slog3_after_primaries"),
                primariesGrade({{gk::AgxOn, 1.0}, {gk::AgxInput, 4.0}, {gk::Saturation, 1.3},
                    {gk::AgxExposure, -1.0}})});
            out.append({QStringLiteral("agx_linear_applelog"),
                primariesGrade({{gk::AgxOn, 1.0}, {gk::AgxInput, 3.0}})});
        }
        {
            // HDR palette: exposure and tint of each zone, saturation, ranges, Black Offset.
            out.append({QStringLiteral("hdr_global"),
                primariesGrade({{gk::HdrGlobalY, 1.0}, {gk::HdrGlobalR, 0.1}, {gk::HdrGlobalB, -0.05}})});
            out.append({QStringLiteral("hdr_zones"),
                primariesGrade({{gk::HdrShadowY, 2.0}, {gk::HdrShadowG, 0.08}, {gk::HdrLightB, 0.12},
                    {gk::HdrHighlightY, -1.0}, {gk::HdrSpecularSat, 0.3}, {gk::HdrLightRange, 0.5},
                    {gk::HdrLightFalloff, 2.5}, {gk::HdrBlackY, 0.5}, {gk::HdrBlackR, 0.05}})});
            out.append({QStringLiteral("hdr_sat_after_primaries"),
                primariesGrade({{gk::HdrGlobalSat, 0.4}, {gk::HdrShadowSat, 1.8}, {gk::Contrast, 1.2},
                    {gk::HdrShadowRange, 0.0}})});
        }
        {
            // Color Warper: Hue - Sat, Chroma - Luma, both with the curves and a LUT-free chain.
            ClipGrade hs = primariesGrade({});
            ClipGrade cl = primariesGrade({});
            for (int row = 0; row < 5; ++row) {
                for (int col = 0; col < 12; ++col)
                    hs.nodes[0].warp.setPin(WarpGrid::HueSat, col, row,
                        (col % 2 ? 0.07f : -0.07f), (row % 2 ? 0.15f : -0.1f));
                for (int col = 0; col < 5; ++col)
                    cl.nodes[0].warp.setPin(WarpGrid::ChromaLuma, col, row,
                        (col % 2 ? 0.15f : -0.1f), (row % 2 ? 0.2f : -0.15f));
            }
            out.append({QStringLiteral("warp_hue_sat"), hs});
            out.append({QStringLiteral("warp_chroma_luma"), cl});
            ClipGrade both = hs;
            both.nodes[0].warp.chromaLuma = cl.nodes[0].warp.chromaLuma;
            both.nodes[0].params.setBase(gk::Saturation, 1.3);
            out.append({QStringLiteral("warp_both_after_primaries"), both});
            ClipGrade high = primariesGrade({});
            high.nodes[0].warp.resolution = 2;
            for (int row = 1; row < 8; row += 2)
                for (int col = 0; col < 24; col += 3)
                    high.nodes[0].warp.setPin(WarpGrid::HueSat, col, row, 0.03f, 0.12f);
            out.append({QStringLiteral("warp_high_resolution"), high});
        }
        {
            ClipGrade g = primariesGrade({});
            g.nodes[0].lutPath = m_lut3d;
            out.append({QStringLiteral("lut3d_tetrahedral"), g});
            g.nodes[0].lutInterpolation = LutInterpolation::Trilinear;
            out.append({QStringLiteral("lut3d_trilinear"), g});
            g.nodes[0].lutPath = m_lut1d;
            out.append({QStringLiteral("lut1d"), g});
        }
        {
            // Serial: a warm node, then a cooling node at half mix; a bypassed node at the end.
            ClipGrade g = primariesGrade({{gk::Temperature, 0.6}});
            const quint64 b = go::addSerialNode(g, g.nodes[0].id);
            g.node(b)->params.setBase(gk::Temperature, -0.8);
            g.node(b)->params.setBase(gk::Mix, 0.5);
            const quint64 c = go::addSerialNode(g, b);
            g.node(c)->params.setBase(gk::Saturation, 0.0);
            g.node(c)->enabled = false;
            out.append({QStringLiteral("serial_mix_bypass"), g});
        }
        {
            // Parallel: two corrections averaged; one keyed.
            ClipGrade g = primariesGrade({{gk::GainR, 1.5}});
            const quint64 a = g.nodes[0].id;
            const quint64 p = go::addParallelNode(g, a);
            g.node(p)->params.setBase(gk::Saturation, 0.0);
            go::addWindow(g, p, GradeWindowShape::Circle);
            out.append({QStringLiteral("parallel"), g});
        }
        {
            // Layer: a keyed top layer over a graded bottom; a second layer bypassed.
            ClipGrade g = primariesGrade({{gk::Contrast, 1.3}});
            const quint64 a = g.nodes[0].id;
            const quint64 top = go::addLayerNode(g, a);
            g.node(top)->params.setBase(gk::Saturation, 0.0);
            g.node(top)->params.setBase(gk::Mix, 0.8);
            g.node(top)->qualifier.enabled = true;
            g.node(top)->qualifier.hueCenter = 0.6;
            g.node(top)->qualifier.hueWidth = 0.3;
            g.node(top)->qualifier.satLow = 0.0;
            const quint64 off = go::addLayerNode(g, a);
            g.node(off)->params.setBase(gk::GainR, 3.0);
            g.node(off)->enabled = false;
            out.append({QStringLiteral("layer"), g});
        }
        {
            // Primaries extras, Log palette, Soft Clip and Sat vs Lum (roadmap: Color 1.12).
            out.append({QStringLiteral("boost"),
                primariesGrade({{gk::Saturation, 0.5}, {gk::ColorBoost, 100.0}})});
            out.append({QStringLiteral("boost_negative"),
                primariesGrade({{gk::Saturation, 0.5}, {gk::ColorBoost, -100.0}})});
            out.append({QStringLiteral("shadows_highlights"),
                primariesGrade({{gk::Shadows, 70.0}, {gk::Highlights, -60.0}})});
            out.append({QStringLiteral("shadows_crush"),
                primariesGrade({{gk::Shadows, -80.0}, {gk::Highlights, 80.0}})});
            out.append({QStringLiteral("midtone_detail"),
                primariesGrade({{gk::MidtoneDetail, 100.0}, {gk::Contrast, 1.25}})});
            out.append({QStringLiteral("midtone_soften"),
                primariesGrade({{gk::MidtoneDetail, -100.0}, {gk::Contrast, 1.2}})});
            out.append({QStringLiteral("log_wheels"),
                primariesGrade({{gk::LogShadowR, 0.15}, {gk::LogMidB, -0.1}, {gk::LogHighG, 0.12},
                    {gk::LogHighY, -0.05}, {gk::LogOffsetR, 0.04}, {gk::LogLowRange, 0.4},
                    {gk::LogHighRange, 0.7}})});
            out.append({QStringLiteral("soft_clip"),
                primariesGrade({{gk::SoftClipLow, 0.15}, {gk::SoftClipLowSoft, 0.3},
                    {gk::SoftClipHigh, 0.8}, {gk::SoftClipHighSoft, 0.4}})});
            ClipGrade g = primariesGrade({});
            g.nodes[0].curves[GradeCurve::SatLum] = {{0.3, 0.3}, {0.9, 0.7}};
            g.nodes[0].curves[GradeCurve::HueLum] = {{0.2, 0.6}};
            out.append({QStringLiteral("sat_vs_lum"), g});
        }
        return out;
    }

private slots:
    void gradeLutMatchesTheGrade()
    {
        // Generate 3D LUT: the table, read back with the CUBE reader, gives the grade's colours
        // (windows and qualifiers left out).
        ClipGrade grade = primariesGrade({{gk::Saturation, 1.3}, {gk::Contrast, 1.2},
            {gk::GainR, 1.1}, {gk::ColorBoost, 40.0}, {gk::Shadows, 30.0}, {gk::LogMidB, 0.05}});
        grade.nodes[0].curves[GradeCurve::Luma] = {{0.25, 0.2}, {0.75, 0.8}};
        go::addWindow(grade, grade.nodes[0].id, GradeWindowShape::Circle);
        const QByteArray text = gradeToCube(grade, 0, 33, QStringLiteral("test \"grade\""));
        CubeLut lut;
        QString error;
        QVERIFY2(lut.parse(text, &error), qPrintable(error));
        QVERIFY(lut.is3d());
        QCOMPARE(lut.size(), 33);
        QCOMPARE(lut.title(), QStringLiteral("test grade"));
        ClipGrade flat = grade;
        flat.nodes[0].windows.clear();
        const PreparedGrade prepared = prepareGrade(flat, 0, grademath::PictureGeometry {});
        std::vector<float> scratch;
        double worst = 0.0;
        for (const std::array<float, 3> c : {std::array {0.1f, 0.5f, 0.9f},
                 std::array {0.8f, 0.3f, 0.2f}, std::array {0.5f, 0.5f, 0.5f},
                 std::array {0.05f, 0.1f, 0.02f}}) {
            float want[3] = {c[0], c[1], c[2]};
            gradePixel(prepared, want, 0.5f, 0.5f, scratch);
            float got[3] = {c[0], c[1], c[2]};
            lut.applyTetrahedral(got);
            for (int ch = 0; ch < 3; ++ch)
                worst = std::max(worst, double(std::abs(got[ch] - std::clamp(want[ch], 0.0f, 1.0f))));
        }
        QVERIFY2(worst < 0.02, qPrintable(QString::number(worst)));
        // An empty grade is the identity.
        CubeLut id;
        QVERIFY(id.parse(gradeToCube(ClipGrade {}, 0, 17, QStringLiteral("id")), &error));
        float c[3] = {0.3f, 0.6f, 0.9f};
        id.applyTetrahedral(c);
        QVERIFY(std::abs(c[0] - 0.3f) < 1e-4f && std::abs(c[1] - 0.6f) < 1e-4f
            && std::abs(c[2] - 0.9f) < 1e-4f);
        // Written to a file and loaded back like a LUT node does.
        const QString path = m_dir.filePath(QStringLiteral("generated.cube"));
        QVERIFY2(writeGradeCube(grade, 0, 17, QStringLiteral("file"), path, &error),
            qPrintable(error));
        QVERIFY(loadCubeLut(path, &error));
    }

    void gradeLutAppliedAsLutNodeMatchesGradeOnGpu()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const ClipGrade grade = primariesGrade({{gk::Saturation, 1.3}, {gk::Contrast, 1.15},
            {gk::GainB, 0.9}, {gk::LogHighG, 0.05}});
        const QString path = m_dir.filePath(QStringLiteral("baked.cube"));
        QString error;
        QVERIFY2(writeGradeCube(grade, 0, 33, QStringLiteral("baked"), path, &error),
            qPrintable(error));
        ClipGrade lutGrade = primariesGrade({});
        lutGrade.nodes[0].lutPath = path;
        const QImage direct = render(*m_gpu, timelineWith(grade));
        const QImage viaLut = render(*m_gpu, timelineWith(lutGrade));
        QVERIFY2(maxDiff(direct, viaLut) <= 4, qPrintable(QString::number(maxDiff(direct, viaLut))));
    }

    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
        // The test picture: hue across, lightness down, plus a grey and a white strip.
        QImage ramp(kW, kH, QImage::Format_RGB32);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                QColor c = QColor::fromHslF(double(x) / kW, y < 4 ? 0.0 : 0.85,
                    std::clamp(0.08 + 0.84 * double(y) / (kH - 1), 0.0, 1.0));
                if (y >= kH - 3)
                    c = QColor(255, 255, 255);
                ramp.setPixelColor(x, y, c);
            }
        }
        const QString path = m_dir.filePath(QStringLiteral("ramp.png"));
        QVERIFY(ramp.save(path));
        auto media = std::make_shared<MediaPathMap>();
        media->insert(QStringLiteral("ramp"), path);
        m_gpu->setMediaPaths(media);
        m_cpu->setMediaPaths(media);
        m_lut3d = m_dir.filePath(QStringLiteral("look.cube"));
        m_lut1d = m_dir.filePath(QStringLiteral("curve1d.cube"));
        for (const auto& [file, text] :
            {std::pair {m_lut3d, cube3d(9)}, std::pair {m_lut1d, cube1d(32)}}) {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(text);
        }
    }

    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    void gpuMatchesCpuReference()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QImage plain = render(*m_gpu, timelineWith({}));
        QCOMPARE(plain.size(), QSize(kW, kH));
        for (const auto& [name, grade] : cases()) {
            const QImage graded = render(*m_gpu, timelineWith(grade));
            QImage reference = plain.copy();
            applyGrade(grade, 0, reference);
            QPoint at;
            const int d = maxDiff(graded, reference, &at);
            const int changed = maxDiff(graded, plain);
            qInfo("%s: GPU vs CPU reference max diff %d at (%d, %d); changed by up to %d",
                qPrintable(name), d, at.x(), at.y(), changed);
            QVERIFY2(d <= 2, qPrintable(name));
            QVERIFY2(changed >= 10, qPrintable(name)); // the case really grades something
        }
    }

    void matteFinesseChangesThePicture()
    {
        // Spec 108: the finesse cases above really differ from the plain qualifier (on the
        // GPU path as well as on the CPU reference), so the match is not two ignored values.
        if (!gpuReady())
            QSKIP("no OpenGL");
        ClipGrade base;
        QHash<QString, ClipGrade> byName;
        for (const auto& [name, grade] : cases())
            byName.insert(name, grade);
        const QImage plain = render(*m_gpu, timelineWith(byName.value(QStringLiteral("qualifier"))));
        for (const QString& name : {QStringLiteral("matte_blur"), QStringLiteral("matte_shrink"),
                 QStringLiteral("matte_grow"), QStringLiteral("matte_all"),
                 QStringLiteral("matte_blur_in")}) {
            QVERIFY2(byName.contains(name), qPrintable(name));
            ClipGrade g = byName.value(name);
            // Same qualifier as "qualifier" but for the finesse: compare to the unfinessed twin.
            GradeQualifier twin = g.nodes[0].qualifier;
            twin.cleanBlack = twin.cleanWhite = twin.shrinkGrow = twin.blurRadius = 0.0;
            twin.inOutRatio = twin.denoise = 0.0;
            ClipGrade untouched = g;
            untouched.nodes[0].qualifier = twin;
            const int d = maxDiff(render(*m_gpu, timelineWith(g)), render(*m_gpu, timelineWith(untouched)));
            qInfo("%s: finesse changes the GPU picture by up to %d", qPrintable(name), d);
            QVERIFY2(d >= 8, qPrintable(name));
        }
        Q_UNUSED(plain)
    }

    void cpuBackendMatchesReference()
    {
        const QImage plain = render(*m_cpu, timelineWith({}));
        QCOMPARE(plain.size(), QSize(kW, kH));
        for (const auto& [name, grade] : cases()) {
            const QImage graded = render(*m_cpu, timelineWith(grade));
            QImage reference = plain.copy();
            applyGrade(grade, 0, reference);
            QVERIFY2(maxDiff(graded, reference) <= 1, qPrintable(name));
        }
    }

    void blurWindowOnlyBlursInside()
    {
        // The owner's tutorial: a circle window, Radius up: only inside the window blurs; the
        // inverted window blurs everything else.
        QImage stripes(96, 96, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 96; ++x)
                stripes.setPixelColor(x, y, ((x / 2 + y / 2) % 2) ? Qt::white : Qt::black);
        ClipGrade g = primariesGrade({{gk::BlurRadiusR, 1.0}, {gk::BlurRadiusG, 1.0},
            {gk::BlurRadiusB, 1.0}, {gk::BlurScaling, 1.0}});
        const quint64 circle = go::addWindow(g, g.nodes[0].id, GradeWindowShape::Circle);
        g.window(circle)->params.setBase(wk::Width, 0.3);
        g.window(circle)->params.setBase(wk::Height, 0.3);
        g.window(circle)->params.setBase(wk::SoftOut, 0.01);
        auto spread = [](const QImage& img, const QRect& r) {
            int lo = 255, hi = 0;
            for (int y = r.top(); y < r.bottom(); ++y)
                for (int x = r.left(); x < r.right(); ++x) {
                    lo = std::min(lo, qRed(img.pixel(x, y)));
                    hi = std::max(hi, qRed(img.pixel(x, y)));
                }
            return hi - lo;
        };
        const QRect inside(40, 40, 16, 16), outside(2, 2, 16, 16);
        QImage graded = stripes.copy();
        applyGrade(g, 0, graded);
        QVERIFY(spread(graded, inside) < 40); // blurred to grey
        QCOMPARE(spread(graded, outside), 255); // untouched
        g.window(circle)->invert = true;
        graded = stripes.copy();
        applyGrade(g, 0, graded);
        QCOMPARE(spread(graded, inside), 255);
        QVERIFY(spread(graded, outside) < 40);
        // Radius 0.5 (the default) changes nothing, whatever the window.
        QImage same = stripes.copy();
        applyGrade(primariesGrade({}), 0, same);
        QCOMPARE(same, stripes);
    }

    void gpuAndCpuBackendsAgree()
    {
        // Spec 44a: the CPU fallback gives the same pixels as the GPU within a small tolerance.
        if (!gpuReady())
            QSKIP("no OpenGL");
        const int base
            = maxDiff(render(*m_gpu, timelineWith({})), render(*m_cpu, timelineWith({})));
        qInfo("ungraded GPU vs CPU backend: max diff %d", base);
        QVERIFY(base <= 2);
        for (const auto& [name, grade] : cases()) {
            const int d
                = maxDiff(render(*m_gpu, timelineWith(grade)), render(*m_cpu, timelineWith(grade)));
            qInfo("%s: GPU vs CPU backend max diff %d", qPrintable(name), d);
            QVERIFY2(d <= base + 3, qPrintable(name));
        }
    }

    void outputToneMapping()
    {
        // The project's tone mapping runs after the timeline grade on both back ends and equals
        // the same grade as a node; off, the picture is bit-identical.
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            if (c == m_gpu.get() && !gpuReady())
                continue;
            TimelineData tl = timelineWith({});
            const QImage plain = render(*c, tl);
            tl.toneMap = ToneMap {};
            QCOMPARE(render(*c, tl), plain);
            tl.toneMap.agx = true;
            tl.toneMap.look = AgxLook::Punchy;
            const QImage mapped = render(*c, tl);
            QVERIFY(maxDiff(mapped, plain) >= 10);
            QImage reference = plain.copy();
            applyGrade(gradeops::toneMapGrade(tl.toneMap), 0, reference);
            QVERIFY(maxDiff(mapped, reference) <= 2);
            // Bypassing the grades bypasses the mapping too.
            QCOMPARE(render(*c, tl, true), plain);
            // It follows the timeline grade.
            ClipGrade grey = primariesGrade({{gk::Saturation, 0.0}});
            TimelineData both = timelineWith({}, grey);
            both.toneMap = tl.toneMap;
            QImage chain = render(*c, timelineWith({}, grey));
            applyGrade(gradeops::toneMapGrade(tl.toneMap), 0, chain);
            QVERIFY(maxDiff(render(*c, both), chain) <= 2);
        }
    }

    void timelineGradeOverComposite()
    {
        // The timeline grade sees the composite of every track: a solid on V2 over half the
        // frame turns grey with the picture below.
        ClipGrade grey = primariesGrade({{gk::Saturation, 0.0}});
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            if (c == m_gpu.get() && !gpuReady())
                continue;
            TimelineData tl = timelineWith({}, grey);
            ClipData solid = solidClip(tl, QColor(255, 0, 0), 0, 10);
            solid.transform.setBase(keys::ZoomX, 0.5);
            solid.transform.setBase(keys::ZoomY, 0.5);
            addClip(tl, TrackType::Video, 1, std::move(solid));
            const QImage img = render(*c, tl);
            const Px centre = px(img, kW / 2, kH / 2); // the (grey now) red solid
            QVERIFY2(std::abs(centre.r - centre.g) <= 2 && std::abs(centre.g - centre.b) <= 2,
                describe(centre).constData());
            QVERIFY(centre.r > 40 && centre.r < 70); // red's luma: 0.2126 x 255 = 54
            const Px corner = px(img, 2, 20);
            QVERIFY(std::abs(corner.r - corner.b) <= 2);
            // Bypass shows the colours again.
            const Px bypassed = px(render(*c, tl, true), kW / 2, kH / 2);
            QVERIFY(bypassed.r > 240 && bypassed.g < 15);
        }
    }

    void gradeComesAfterEditPageEffects()
    {
        // Invert (an Edit page effect), then the grade's red gain 0: no red anywhere. Graded
        // first and inverted after, red would be 255 everywhere.
        ClipGrade noRed = primariesGrade({{gk::GainR, 0.0}});
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            if (c == m_gpu.get() && !gpuReady())
                continue;
            TimelineData tl = timelineWith(noRed);
            tl.videoTracks[0].clips[0].effects.append(EffectRegistry::instance().createEffect(
                QLatin1String(fxid::Invert), tl.allocateId()));
            const QImage img = render(*c, tl);
            int maxRed = 0;
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x)
                    maxRed = std::max(maxRed, qRed(img.pixel(x, y)));
            QVERIFY2(maxRed <= 2, qPrintable(QString::number(maxRed)));
        }
    }

    void windowsMoveWithTheClip()
    {
        // A small circle window at the clip's centre: with the clip moved right and scaled
        // down, the graded spot moves with it (windows live in clip content coordinates).
        ClipGrade spot = primariesGrade({{gk::GainY, 0.0}, {gk::LumMix, 0.0}}); // black
        go::addWindow(spot, spot.nodes[0].id, GradeWindowShape::Circle);
        spot.window(spot.allocateId() - 1)->params.setBase(wk::Width, 0.2);
        spot.window(spot.allocateId() - 1)->params.setBase(wk::Height, 0.2);
        spot.window(spot.allocateId() - 1)->params.setBase(wk::SoftOut, 0.0);
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            if (c == m_gpu.get() && !gpuReady())
                continue;
            TimelineData tl = timelineWith(spot);
            ClipData& clip = tl.videoTracks[0].clips[0];
            clip.transform.setBase(keys::ZoomX, 0.5);
            clip.transform.setBase(keys::ZoomY, 0.5);
            clip.transform.setBase(keys::PosX, 16.0);
            const QImage img = render(*c, tl);
            // The clip's centre is at (48, 18): black there, picture around it.
            const Px centre = px(img, 48, 18);
            QVERIFY2(centre.r < 10 && centre.g < 10 && centre.b < 10, describe(centre).constData());
            const Px beside = px(img, 38, 12);
            QVERIFY2(beside.r + beside.g + beside.b > 60, describe(beside).constData());
            const Px frameCentre = px(img, 35, 18); // inside the clip, left of the spot
            QVERIFY(frameCentre.r + frameCentre.g + frameCentre.b > 60);
        }
    }

    void keyframedGradeAnimates()
    {
        ClipGrade g = primariesGrade({});
        AnimatedValue& gain = g.nodes[0].params.animated(gk::GainY, 1.0);
        gain.setKey(0, 0.0);
        gain.setKey(8, 1.0);
        g.nodes[0].params.setBase(gk::LumMix, 0.0);
        const TimelineData tl = timelineWith(g);
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            if (c == m_gpu.get() && !gpuReady())
                continue;
            const Px dark = px(render(*c, tl, false, 0), 30, 30);
            const Px lit = px(render(*c, tl, false, 8), 30, 30);
            QVERIFY(dark.r + dark.g + dark.b < 6);
            QVERIFY(lit.r + lit.g + lit.b > 200);
        }
    }

    void shapeKeysMatchOnGpuAndCpu()
    {
        // Spec 111: a keyframed polygon window renders the same on both backends at the keys
        // and between them, and the shape really changes with the frame.
        if (!gpuReady())
            QSKIP("no OpenGL");
        ClipGrade g = primariesGrade({{gk::Saturation, 0.0}});
        const quint64 id = go::addWindow(g, g.nodes[0].id, GradeWindowShape::Polygon);
        GradeWindow* w = g.window(id);
        w->params.setBase(wk::SoftOut, 0.0);
        w->shapeKeys.insert(0, w->points);
        GradeWindowShapePoints moved = w->points;
        for (GradeWindowPoint& p : moved)
            p.pos += QPointF(0.4, 0.0);
        w->shapeKeys.insert(8, moved);
        const TimelineData tl = timelineWith(g);
        const QImage plain = render(*m_gpu, timelineWith({}));
        QImage first, last;
        for (const int64_t frame : {0, 4, 8}) {
            const QImage graded = render(*m_gpu, tl, false, frame);
            QImage reference = render(*m_cpu, timelineWith({}), false, frame);
            applyGrade(g, frame, reference);
            QVERIFY2(maxDiff(graded, reference) <= 2, qPrintable(QString::number(frame)));
            if (frame == 0)
                first = graded;
            if (frame == 8)
                last = graded;
        }
        QVERIFY(maxDiff(first, last) > 10);
        QVERIFY(maxDiff(first, plain) > 10);
    }

    void bypassAndIdentity()
    {
        const ClipGrade g = primariesGrade({{gk::GainR, 0.0}});
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            if (c == m_gpu.get() && !gpuReady())
                continue;
            const QImage plain = render(*c, timelineWith({}));
            QCOMPARE(maxDiff(render(*c, timelineWith(g), true), plain), 0);
            // A neutral grade renders exactly as no grade.
            QCOMPARE(maxDiff(render(*c, timelineWith(primariesGrade({}))), plain), 0);
        }
    }

    void sharedImageGradesEveryRow()
    {
        // Regression: the row threads of applyGrade() each detached a shared image (the CPU
        // backend grades pictures that a cache still holds), so some rows were lost.
        QImage source(400, 300, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(120, 60, 30));
        const ClipGrade g = primariesGrade({{gk::Saturation, 0.0}});
        QImage single = source.copy(0, 0, 400, 1);
        applyGrade(g, 0, single);
        const QRgb expected = single.pixel(0, 0);
        for (int run = 0; run < 20; ++run) {
            QImage shared = source; // shares the data with source
            applyGrade(g, 0, shared);
            QCOMPARE(source.pixel(0, 0), qRgb(120, 60, 30)); // the original stays untouched
            for (int y = 0; y < shared.height(); ++y)
                QVERIFY2(shared.pixel(200, y) == expected, qPrintable(QString::number(y)));
        }
    }

    void renderCacheKeys()
    {
        TimelineData tl = timelineWith({});
        const MediaKeyMap keys;
        const CacheKeyOptions options;
        const auto plain = renderCacheChunks(tl, keys, options);
        QVERIFY(!plain.empty());
        QCOMPARE(plain[0].reasons & CacheEffects, 0u);
        TimelineData graded = timelineWith(primariesGrade({{gk::GainR, 1.2}}));
        const auto withGrade = renderCacheChunks(graded, keys, options);
        QVERIFY(withGrade[0].key != plain[0].key);
        QVERIFY(withGrade[0].reasons & CacheEffects); // Smart mode caches graded clips
        TimelineData timelineGraded = timelineWith({}, primariesGrade({{gk::Saturation, 0.5}}));
        QVERIFY(renderCacheChunks(timelineGraded, keys, options)[0].key != plain[0].key);
        // A changed LUT file changes the key too.
        TimelineData lut = timelineWith(primariesGrade({}));
        lut.videoTracks[0].clips[0].grade.nodes[0].lutPath = m_lut1d;
        const QByteArray before = renderCacheChunks(lut, keys, options)[0].key;
        QFile f(m_lut1d);
        QVERIFY(f.open(QIODevice::Append));
        f.write("# changed\n");
        f.close();
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(
            QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime));
        f.close();
        QVERIFY(renderCacheChunks(lut, keys, options)[0].key != before);
        // samePictures() sees the timeline grade.
        QVERIFY(!render::samePictures(tl, timelineGraded));
    }

    // ---- Input LUT of the media item (Media Pool > 3D LUT, ClipAttributes::inputLut) ----

    TimelineData timelineWithInputLut(const QString& lut, const ClipGrade& clipGrade = {})
    {
        TimelineData tl = timelineWith(clipGrade);
        tl.videoTracks[0].clips[0].attributes.inputLut = lut;
        return tl;
    }

    void inputLutMatchesTheCubeOnBothBackends()
    {
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            if (c == m_gpu.get() && !gpuReady())
                continue;
            const QImage plain = render(*c, timelineWith({}));
            const QImage withLut = render(*c, timelineWithInputLut(m_lut3d));
            QVERIFY(maxDiff(plain, withLut) > 20); // the look is strong: it shows
            // The reference: the same LUT as a node of a grade on the plain picture.
            ClipGrade asNode = primariesGrade({});
            asNode.nodes[0].lutPath = m_lut3d;
            QImage reference = plain.copy();
            applyGrade(asNode, 0, reference);
            const int d = maxDiff(withLut, reference);
            QVERIFY2(d <= 4, qPrintable(QString::number(d)));
            // A 1D table works as well.
            QVERIFY(maxDiff(plain, render(*c, timelineWithInputLut(m_lut1d))) > 20);
        }
    }

    void inputLutGpuAndCpuAgree()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        for (const QString& lut : {m_lut3d, m_lut1d}) {
            const int d = maxDiff(render(*m_gpu, timelineWithInputLut(lut)),
                render(*m_cpu, timelineWithInputLut(lut)));
            qInfo("input LUT %s: GPU vs CPU backend max diff %d", qPrintable(lut), d);
            QVERIFY(d <= 5);
        }
    }

    void inputLutComesBeforeTheNodeGraph()
    {
        // The LUT is applied first, then the nodes: the same as a LUT-only node feeding the grade.
        const ClipGrade grade = primariesGrade({{gk::Saturation, 0.4}, {gk::GainR, 1.25}});
        ClipGrade chain = grade;
        GradeNode first;
        first.id = chain.allocateId();
        first.lutPath = m_lut3d;
        first.inputs = {0};
        chain.nodes[0].inputs = {first.id};
        chain.nodes.prepend(first);
        chain.output = grade.output;
        QImage reference = render(*m_cpu, timelineWith({}));
        applyGrade(chain, 0, reference);
        ClipGrade swapped = primariesGrade({});
        swapped.nodes[0].lutPath = m_lut3d;
        ClipGrade lutOnly = primariesGrade({});
        lutOnly.nodes[0].lutPath = m_lut3d;
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            if (c == m_gpu.get() && !gpuReady())
                continue;
            const QImage got = render(*c, timelineWithInputLut(m_lut3d, grade));
            const int d = maxDiff(got, reference);
            QVERIFY2(d <= 5, qPrintable(QString::number(d)));
            // Not the other order (grade, then LUT): that picture differs.
            QImage other = render(*m_cpu, timelineWith(grade));
            applyGrade(swapped, 0, other);
            QVERIFY(maxDiff(got, other) > 5);
            // The bypass of the grades (Shift+D) keeps the input LUT: it is the picture's own.
            const QImage bypassed = render(*c, timelineWithInputLut(m_lut3d, grade), true);
            QImage onlyLut = render(*m_cpu, timelineWith({}));
            applyGrade(lutOnly, 0, onlyLut);
            QVERIFY(maxDiff(bypassed, onlyLut) <= 4);
        }
    }

    void inputLutUnreadableFileIsIgnored()
    {
        const QString missing = m_dir.filePath(QStringLiteral("missing.cube"));
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            if (c == m_gpu.get() && !gpuReady())
                continue;
            QCOMPARE(
                maxDiff(render(*c, timelineWithInputLut(missing)), render(*c, timelineWith({}))), 0);
        }
    }

    void inputLutIsPartOfTheCacheKeyAndThePictures()
    {
        const MediaKeyMap keys;
        const CacheKeyOptions options;
        const TimelineData plain = timelineWith({});
        const TimelineData withLut = timelineWithInputLut(m_lut3d);
        const TimelineData otherLut = timelineWithInputLut(m_lut1d);
        const QByteArray k0 = renderCacheChunks(plain, keys, options)[0].key;
        const QByteArray k1 = renderCacheChunks(withLut, keys, options)[0].key;
        QVERIFY(k0 != k1);
        QVERIFY(k1 != renderCacheChunks(otherLut, keys, options)[0].key);
        QCOMPARE(renderCacheChunks(timelineWithInputLut(m_lut3d), keys, options)[0].key, k1);
        QVERIFY(!render::samePictures(plain, withLut));
        // A changed LUT file changes the key (size and time count).
        QFile f(m_lut3d);
        QVERIFY(f.open(QIODevice::Append));
        f.write("# changed\n");
        f.close();
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(
            QDateTime::currentDateTime().addSecs(9), QFileDevice::FileModificationTime));
        f.close();
        QVERIFY(renderCacheChunks(withLut, keys, options)[0].key != k1);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestGradeRender>(argc, argv);
}

#include "test_graderender.moc"
