// SPDX-License-Identifier: GPL-3.0-or-later
// Color page core (docs/color-page.md, spec 44a): the grade maths against reference values
// (primaries, curves, HSL curves, qualifier, windows), the node graph operations, keyframe
// paths, edits that move keys, serialization (format 16), migration of format 15 files,
// undo through Project and the gallery.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/GradeGallery.h"
#include "core/GradeMath.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>

using namespace kk;
using namespace kk::test;
using namespace kk::grademath;
namespace go = kk::gradeops;
namespace gk = kk::gradekeys;
namespace wk = kk::gradewindowkeys;

namespace {

bool near(double a, double b, double eps = 1e-4)
{
    return std::abs(a - b) <= eps;
}

#define KK_COMPARE_RGB(c, r, g, b, eps)                                                            \
    QVERIFY2(near((c)[0], r, eps) && near((c)[1], g, eps) && near((c)[2], b, eps),                 \
        qPrintable(QStringLiteral("got %1 %2 %3").arg((c)[0]).arg((c)[1]).arg((c)[2])))

/// Primaries of a node with @p values set.
PrimaryValues primaries(std::initializer_list<std::pair<const QString, double>> values)
{
    GradeNode n;
    for (const auto& [k, v] : values)
        n.params.setBase(k, v);
    return PrimaryValues::at(n, 0);
}

/// Curve tables with one curve set to @p points.
CurveTables tablesWith(GradeCurve c, CurvePoints points)
{
    GradeCurves curves;
    curves[c] = std::move(points);
    return CurveTables::bake(curves);
}

/// A grade using every feature: serial, parallel and layer nodes, keyed primaries, curves,
/// qualifier, every window shape, a LUT path and a bypassed node.
ClipGrade richGrade()
{
    ClipGrade g;
    const quint64 a = go::initialize(g);
    GradeNode* n = g.node(a);
    n->params.values[gk::GainR].setKey(0, 1.0);
    n->params.values[gk::GainR].setKey(24, 1.5);
    n->params.setBase(gk::Saturation, 1.2);
    n->label = QStringLiteral("Balance");
    const quint64 b = go::addSerialNode(g, a);
    g.node(b)->curves[GradeCurve::Luma] = {{0.25, 0.2}, {0.75, 0.8}};
    g.node(b)->curves[GradeCurve::HueSat] = {{0.0, 0.5}, {0.33, 0.9}};
    g.node(b)->curves[GradeCurve::SatSat] = {{0.5, 0.4}};
    g.node(b)->lutPath = QStringLiteral("/luts/look.cube");
    g.node(b)->lutInterpolation = LutInterpolation::Trilinear;
    const quint64 c = go::addParallelNode(g, b);
    g.node(c)->qualifier.enabled = true;
    g.node(c)->qualifier.hueCenter = 0.05;
    g.node(c)->params.setBase(gk::Mix, 0.5);
    const quint64 w1 = go::addWindow(g, c, GradeWindowShape::Circle);
    g.window(w1)->params.values[wk::CenterX].setKey(0, 0.3);
    g.window(w1)->params.values[wk::CenterX].setKey(10, 0.7);
    go::addWindow(g, c, GradeWindowShape::Linear);
    const quint64 w3 = go::addWindow(g, c, GradeWindowShape::Polygon);
    g.window(w3)->invert = true;
    go::addWindow(g, c, GradeWindowShape::Gradient);
    const quint64 d = go::addLayerNode(g, c);
    g.node(d)->enabled = false;
    return g;
}

} // namespace

class TestGrade : public QObject
{
    Q_OBJECT
private slots:
    // ---- Primaries ----

    void liftGammaGainReference()
    {
        QCOMPARE(liftGammaGain(0.5f, 0, 0, 2), 1.0f);
        QVERIFY(near(liftGammaGain(0.0f, 0.2f, 0, 1), 0.2));
        QVERIFY(near(liftGammaGain(1.0f, 0.2f, 0, 1), 1.0)); // lift leaves white alone
        QVERIFY(near(liftGammaGain(0.25f, 0, 0.5f, 1), 0.5)); // exponent 2^-1
        QVERIFY(near(liftGammaGain(0.25f, 0, -0.5f, 1), 0.0625)); // exponent 2
        QCOMPARE(liftGammaGain(-0.5f, 0, 0.3f, 1), 0.0f); // negative input clamps before pow
    }

    void primariesReference()
    {
        float c[3] = {0.2f, 0.4f, 0.6f};
        const PrimaryValues neutral = primaries({});
        QVERIFY(neutral.isIdentity());
        applyPrimaries(neutral, c);
        KK_COMPARE_RGB(c, 0.2, 0.4, 0.6, 1e-6);

        float o[3] = {0.2f, 0.3f, 0.4f};
        applyPrimaries(primaries({{gk::OffsetY, 0.1}, {gk::OffsetR, 0.05}}), o);
        KK_COMPARE_RGB(o, 0.35, 0.4, 0.5, 1e-5);

        float k[3] = {0.75f, 0.5f, 0.25f};
        applyPrimaries(primaries({{gk::Contrast, 2.0}, {gk::Pivot, 0.5}}), k);
        KK_COMPARE_RGB(k, 1.0, 0.5, 0.0, 1e-5);

        float s[3] = {0.2f, 0.4f, 0.6f};
        const float y = 0.2126f * 0.2f + 0.7152f * 0.4f + 0.0722f * 0.6f;
        applyPrimaries(primaries({{gk::Saturation, 0.0}}), s);
        KK_COMPARE_RGB(s, y, y, y, 1e-5);

        // Hue 120 degrees turns red into green (rotation about the grey axis).
        float h[3] = {1.0f, 0.0f, 0.0f};
        applyPrimaries(primaries({{gk::Hue, 120.0}}), h);
        KK_COMPARE_RGB(h, 0.0, 1.0, 0.0, 1e-5);

        // Temperature +1 on grey: red x 1.25, blue x 0.75; tint +1: green x 0.75.
        float t[3] = {0.5f, 0.5f, 0.5f};
        applyPrimaries(primaries({{gk::Temperature, 1.0}, {gk::Tint, 1.0}}), t);
        KK_COMPARE_RGB(t, 0.625, 0.375, 0.375, 1e-5);

        // Master gain with lum mix 1 (default) adds the luma change to every channel (keeps the
        // colour differences); with lum mix 0 it scales each channel.
        float m1[3] = {0.2f, 0.4f, 0.6f};
        applyPrimaries(primaries({{gk::GainY, 2.0}}), m1);
        KK_COMPARE_RGB(m1, 0.2 + y, 0.4 + y, 0.6 + y, 1e-5);
        float m0[3] = {0.2f, 0.4f, 0.6f};
        applyPrimaries(primaries({{gk::GainY, 2.0}, {gk::LumMix, 0.0}}), m0);
        KK_COMPARE_RGB(m0, 0.4, 0.8, 1.2, 1e-5);

        // Per-channel gain only touches its channel.
        float g[3] = {0.5f, 0.5f, 0.5f};
        applyPrimaries(primaries({{gk::GainB, 0.5}}), g);
        KK_COMPARE_RGB(g, 0.5, 0.5, 0.25, 1e-5);
    }

    void keyframedPrimaries()
    {
        GradeNode n;
        n.params.values[gk::GainG].setKey(0, 1.0);
        n.params.values[gk::GainG].setKey(10, 2.0);
        QCOMPARE(PrimaryValues::at(n, 0).gain[1], 1.0f);
        QCOMPARE(PrimaryValues::at(n, 10).gain[1], 2.0f);
        QVERIFY(PrimaryValues::at(n, 5).gain[1] > 1.0f);
        QVERIFY(!go::isNodeIdentity(n, 5));
        QVERIFY(go::isNodeIdentity(n, 0));
    }

    // ---- Curves ----

    void customCurves()
    {
        QCOMPARE(evaluateCurve({}, GradeCurve::Luma, 0.3), 0.3);
        QCOMPARE(evaluateCurve({}, GradeCurve::HueSat, 0.3), 0.5);
        const CurvePoints s = {{0.5, 0.75}};
        QVERIFY(near(evaluateCurve(s, GradeCurve::Luma, 0.5), 0.75, 1e-9));
        QVERIFY(near(evaluateCurve(s, GradeCurve::Luma, 0.0), 0.0, 1e-9)); // neutral ends
        QVERIFY(near(evaluateCurve(s, GradeCurve::Luma, 1.0), 1.0, 1e-9));
        // Monotone: no overshoot between points.
        double prev = -1;
        for (int i = 0; i <= 100; ++i) {
            const double v = evaluateCurve(s, GradeCurve::Luma, i / 100.0);
            QVERIFY(v >= prev - 1e-12);
            QVERIFY(v >= 0.0 && v <= 1.0);
            prev = v;
        }
        // Hue curves are periodic.
        const CurvePoints hue = {{0.1, 0.8}, {0.6, 0.2}};
        QVERIFY(near(evaluateCurve(hue, GradeCurve::HueHue, 0.0),
            evaluateCurve(hue, GradeCurve::HueHue, 1.0), 1e-9));
        QVERIFY(near(evaluateCurve(hue, GradeCurve::HueHue, 1.1), 0.8, 1e-9));

        // The baked table interpolates the curve.
        const CurveTables t = tablesWith(GradeCurve::Red, s);
        QVERIFY(!t.allIdentity);
        QCOMPARE(t.mask(), 1 << int(GradeCurve::Red));
        QVERIFY(near(t.sample(GradeCurve::Red, 0.5f), 0.75, 1e-3));
        float c[3] = {0.5f, 0.5f, 0.5f};
        applyCurves(t, c);
        KK_COMPARE_RGB(c, 0.75, 0.5, 0.5, 1e-3);

        // The Y curve applies to every channel.
        float y[3] = {0.5f, 0.5f, 0.5f};
        applyCurves(tablesWith(GradeCurve::Luma, s), y);
        KK_COMPARE_RGB(y, 0.75, 0.75, 0.75, 1e-3);
    }

    void addCurvePointAnchorsNeighbours()
    {
        CurvePoints p;
        const int i = addCurvePoint(p, GradeCurve::HueSat, 0.5, 0.9);
        QCOMPARE(p.size(), 3);
        QCOMPARE(p[i].y, 0.9);
        QVERIFY(near(evaluateCurve(p, GradeCurve::HueSat, 0.1), 0.5, 1e-9)); // far away: neutral
        CurvePoints lum;
        addCurvePoint(lum, GradeCurve::Luma, 0.4, 0.6);
        QCOMPARE(lum.size(), 1); // the Y curve gets no extra points
        addCurvePoint(lum, GradeCurve::Luma, 0.4, 0.7); // same x replaces
        QCOMPARE(lum.size(), 1);
        QCOMPARE(lum[0].y, 0.7);
    }

    void hslCurves()
    {
        // Hue vs Sat flat at 0: everything grey (lightness kept).
        float red[3] = {1.0f, 0.0f, 0.0f};
        applyCurves(tablesWith(GradeCurve::HueSat, {{0.5, 0.0}}), red);
        KK_COMPARE_RGB(red, 0.5, 0.5, 0.5, 1e-4);
        // Sat vs Sat flat at 0 likewise.
        float blue[3] = {0.0f, 0.0f, 1.0f};
        applyCurves(tablesWith(GradeCurve::SatSat, {{0.0, 0.0}, {1.0, 0.0}}), blue);
        KK_COMPARE_RGB(blue, 0.5, 0.5, 0.5, 1e-4);
        // Hue vs Hue: +1/3 turn (y = 0.5 + 1/3) turns red into green.
        float r2[3] = {1.0f, 0.0f, 0.0f};
        applyCurves(tablesWith(GradeCurve::HueHue, {{0.5, 0.5 + 1.0 / 3.0}}), r2);
        KK_COMPARE_RGB(r2, 0.0, 1.0, 0.0, 1e-3);
        // Hue vs Lum flat at 1 (factor 2): the luma doubles, colour differences stay.
        float g[3] = {0.3f, 0.2f, 0.1f};
        const float y = luma(g);
        applyCurves(tablesWith(GradeCurve::HueLum, {{0.5, 1.0}}), g);
        KK_COMPARE_RGB(g, 0.3 + y, 0.2 + y, 0.1 + y, 1e-3);
        // Lum vs Sat at 0 for dark colours only: a dark colour greys, a bright one keeps.
        const CurvePoints darkGrey = {{0.0, 0.0}, {0.3, 0.0}, {0.6, 0.5}};
        float dark[3] = {0.2f, 0.0f, 0.0f};
        applyCurves(tablesWith(GradeCurve::LumSat, darkGrey), dark);
        QVERIFY(near(dark[0], dark[1], 1e-4));
        float bright[3] = {1.0f, 1.0f, 0.6f};
        applyCurves(tablesWith(GradeCurve::LumSat, darkGrey), bright);
        KK_COMPARE_RGB(bright, 1.0, 1.0, 0.6, 1e-3);
    }

    void hslRoundTrip()
    {
        for (const auto& c : {std::array {0.9f, 0.1f, 0.3f}, std::array {0.2f, 0.8f, 0.5f},
                 std::array {0.4f, 0.4f, 0.4f}, std::array {0.0f, 0.3f, 1.0f}}) {
            float hsl[3], back[3];
            rgbToHsl(c.data(), hsl);
            hslToRgb(hsl, back);
            KK_COMPARE_RGB(back, c[0], c[1], c[2], 1e-5);
        }
        float hsl[3];
        const float green[3] = {0, 1, 0};
        rgbToHsl(green, hsl);
        QVERIFY(near(hsl[0], 1.0 / 3.0, 1e-6));
        QVERIFY(near(hsl[1], 1.0) && near(hsl[2], 0.5));
    }

    // ---- Qualifier and windows ----

    void qualifierKeys()
    {
        GradeQualifier q;
        const float red[3] = {1, 0, 0}, green[3] = {0, 1, 0}, grey[3] = {0.5f, 0.5f, 0.5f};
        QCOMPARE(qualifierKey(q, green), 1.0f); // disabled = everything
        q.enabled = true;
        q.hueCenter = 0.0;
        QCOMPARE(qualifierKey(q, red), 1.0f);
        QCOMPARE(qualifierKey(q, green), 0.0f);
        QCOMPARE(qualifierKey(q, grey), 0.0f); // saturation 0 < satLow
        q.invert = true;
        QCOMPARE(qualifierKey(q, red), 0.0f);
        QCOMPARE(qualifierKey(q, green), 1.0f);
        q.invert = false;
        // Soft edge: half way through the softness ramp the key is 0.5.
        q.hueWidth = 0.1;
        q.hueSoft = 0.1;
        float hsl[3] = {0.1f, 1.0f, 0.5f}; // 0.05 (half width) + 0.05 (half the softness)
        float rgb[3];
        hslToRgb(hsl, rgb);
        QVERIFY(near(qualifierKey(q, rgb), 0.5, 1e-3));
        // Only lightness.
        q.useHue = q.useSat = false;
        q.lumLow = 0.6;
        q.lumSoft = 0.0;
        QCOMPARE(qualifierKey(q, grey), 0.0f);
        const float light[3] = {0.8f, 0.8f, 0.8f};
        QCOMPARE(qualifierKey(q, light), 1.0f);
    }

    void windowShapes()
    {
        PictureGeometry g;
        g.width = 200;
        g.height = 100;
        GradeNode n;
        ClipGrade grade;
        grade.nodes.append(n);
        grade.nodes[0].id = 1;
        grade.output = 1;
        const quint64 id = go::addWindow(grade, 1, GradeWindowShape::Circle);
        GradeWindow* w = grade.window(id);
        w->params.setBase(wk::SoftIn, 0.0);
        w->params.setBase(wk::SoftOut, 0.0);
        auto matte = [&](double x, double y) {
            return windowsMatte(evaluateWindows(*grade.node(1), 0, g), float(x), float(y));
        };
        // Circle: 100 x 50 px ellipse around the centre (100, 50).
        QCOMPARE(matte(100, 50), 1.0f);
        QCOMPARE(matte(5, 5), 0.0f);
        QVERIFY(near(matte(150, 50), 0.5, 1e-3)); // on the edge (half-pixel antialiasing)
        QCOMPARE(matte(140, 50), 1.0f);
        QCOMPARE(matte(160, 50), 0.0f);
        // Soft edges: inside and outside softness of 10 px (0.1 picture heights).
        w->params.setBase(wk::SoftIn, 0.1);
        w->params.setBase(wk::SoftOut, 0.1);
        QVERIFY(near(matte(150, 50), 0.5, 1e-3));
        const float inside = matte(145, 50), outside = matte(155, 50);
        QVERIFY(inside > 0.5f && inside < 1.0f);
        QVERIFY(outside > 0.0f && outside < 0.5f);
        QVERIFY(near(inside + outside, 1.0, 1e-3)); // symmetric ramp
        // Invert and opacity.
        w->invert = true;
        QCOMPARE(matte(100, 50), 0.0f);
        QCOMPARE(matte(5, 5), 1.0f);
        w->invert = false;
        w->params.setBase(wk::Opacity, 0.5);
        QCOMPARE(matte(100, 50), 0.5f);
        w->params.setBase(wk::Opacity, 1.0);
        w->params.setBase(wk::SoftIn, 0.0);
        w->params.setBase(wk::SoftOut, 0.0);

        // Linear (rectangle) rotated by 90 degrees: 100 x 50 becomes 50 wide, 100 tall... in
        // window units (fractions of the picture): width 0.5 = 100 px, height 0.5 = 50 px.
        w->shape = GradeWindowShape::Linear;
        QCOMPARE(matte(145, 70), 1.0f);
        QCOMPARE(matte(100, 80), 0.0f);
        w->params.setBase(wk::Rotation, 90.0);
        QCOMPARE(matte(100, 70), 1.0f); // now 50 px wide and 100 px tall
        QCOMPARE(matte(145, 50), 0.0f);
        w->params.setBase(wk::Rotation, 0.0);

        // Polygon: the square of the window box equals the rectangle.
        w->shape = GradeWindowShape::Polygon;
        w->points
            = {QPointF(-0.5, -0.5), QPointF(0.5, -0.5), QPointF(0.5, 0.5), QPointF(-0.5, 0.5)};
        QCOMPARE(matte(145, 70), 1.0f);
        QCOMPARE(matte(100, 80), 0.0f);
        QVERIFY(near(matte(150, 50), 0.5, 1e-3));
        // Triangle: the point under the tip is inside, the corners of the box are not.
        w->points = {QPointF(0, -0.5), QPointF(0.5, 0.5), QPointF(-0.5, 0.5)};
        QCOMPARE(matte(100, 60), 1.0f);
        QCOMPARE(matte(55, 30), 0.0f);

        // Gradient: the upper half (before rotation) selected.
        w->shape = GradeWindowShape::Gradient;
        QCOMPARE(matte(10, 10), 1.0f);
        QCOMPARE(matte(10, 90), 0.0f);
        QVERIFY(near(matte(10, 50), 0.5, 1e-3));
        w->params.setBase(wk::Rotation, 180.0);
        QCOMPARE(matte(10, 10), 0.0f);

        // Two windows combine as a union.
        w->shape = GradeWindowShape::Circle;
        w->params.setBase(wk::Rotation, 0.0);
        w->params.setBase(wk::Width, 0.1);
        w->params.setBase(wk::Height, 0.1);
        w->params.setBase(wk::CenterX, 0.1);
        const quint64 second = go::addWindow(grade, 1, GradeWindowShape::Circle);
        grade.window(second)->params.setBase(wk::Width, 0.1);
        grade.window(second)->params.setBase(wk::Height, 0.1);
        grade.window(second)->params.setBase(wk::CenterX, 0.9);
        QCOMPARE(matte(20, 50), 1.0f);
        QCOMPARE(matte(180, 50), 1.0f);
        QCOMPARE(matte(100, 50), 0.0f);
        // A disabled window does not count; a polygon needs three points.
        grade.window(second)->enabled = false;
        QCOMPARE(matte(180, 50), 0.0f);
        GradeWindow line;
        line.shape = GradeWindowShape::Polygon;
        line.points = {QPointF(0, 0), QPointF(1, 1)};
        QVERIFY(!line.isRenderable());
    }

    void layerFormula()
    {
        // layerOver() equals mix(result, graded, alpha) for o = mix(in, graded, alpha).
        const float in[3] = {0.2f, 0.4f, 0.6f};
        const float graded[3] = {0.8f, 0.1f, 0.3f};
        const float alpha = 0.3f;
        float o[3];
        for (int i = 0; i < 3; ++i)
            o[i] = in[i] + (graded[i] - in[i]) * alpha;
        float r[3] = {0.5f, 0.5f, 0.5f};
        layerOver(r, in, o, alpha);
        KK_COMPARE_RGB(
            r, 0.5 + (0.8 - 0.5) * 0.3, 0.5 + (0.1 - 0.5) * 0.3, 0.5 + (0.3 - 0.5) * 0.3, 1e-6);
    }

    // ---- Graph operations ----

    void serialChain()
    {
        ClipGrade g;
        QVERIFY(go::isIdentity(g, 0));
        QCOMPARE(go::defaultSelection(g), quint64(0));
        const quint64 a = go::addSerialNode(g, 0); // an empty grade gets its first node
        QCOMPARE(g.nodes.size(), 1);
        QCOMPARE(g.output, a);
        QVERIFY(go::isIdentity(g, 0));
        const quint64 b = go::addSerialNode(g, a);
        const quint64 c = go::addSerialNode(g, b);
        QCOMPARE(g.output, c);
        QCOMPARE(g.node(c)->inputs, QVector<quint64> {b});
        QCOMPARE(g.node(b)->inputs, QVector<quint64> {a});
        // Inserting after a keeps the order: a -> new -> b -> c.
        const quint64 n = go::addSerialNode(g, a);
        QCOMPARE(g.node(b)->inputs, QVector<quint64> {n});
        QCOMPARE(g.node(n)->inputs, QVector<quint64> {a});
        const std::vector<int> order = go::evaluationOrder(g);
        QCOMPARE(order.size(), size_t(4));
        QCOMPARE(g.nodes[order[0]].id, a);
        QCOMPARE(g.nodes[order[1]].id, n);
        QCOMPARE(g.nodes[order[3]].id, c);
        // Before b: a -> n -> new -> b.
        const quint64 before = go::addSerialNodeBefore(g, b);
        QCOMPARE(g.node(b)->inputs, QVector<quint64> {before});
        QCOMPARE(g.node(before)->inputs, QVector<quint64> {n});
        // Remove keeps the chain connected.
        QVERIFY(go::removeNode(g, before));
        QCOMPARE(g.node(b)->inputs, QVector<quint64> {n});
        QVERIFY(go::removeNode(g, c));
        QCOMPARE(g.output, b);
        QVERIFY(!go::removeNode(g, 999));
        // Bypass and reset.
        g.node(b)->params.setBase(gk::GainR, 2.0);
        QVERIFY(!go::isIdentity(g, 0));
        QVERIFY(go::setNodeEnabled(g, b, false));
        QVERIFY(!go::setNodeEnabled(g, b, false));
        QVERIFY(go::isIdentity(g, 0));
        QVERIFY(go::setNodeEnabled(g, b, true));
        g.node(b)->params.setBase(gk::Mix, 0.0);
        QVERIFY(go::isIdentity(g, 0));
        QVERIFY(go::resetNode(g, b));
        QVERIFY(g.node(b)->params == ParamSet {});
        QVERIFY(g.node(b)->enabled);
    }

    void parallelAndLayerMixers()
    {
        ClipGrade g;
        const quint64 a = go::initialize(g);
        const quint64 p = go::addParallelNode(g, a);
        QCOMPARE(g.nodes.size(), 3);
        const GradeNode* mixer = g.node(g.output);
        QVERIFY(mixer && mixer->type == GradeNodeType::Parallel);
        QCOMPARE(mixer->inputs, (QVector<quint64> {a, p}));
        QCOMPARE(g.node(p)->inputs, QVector<quint64> {0}); // fed from a's input
        // A second parallel node joins the same mixer.
        const quint64 p2 = go::addParallelNode(g, a);
        QCOMPARE(g.node(g.output)->inputs.size(), 3);
        QCOMPARE(g.node(g.output)->inputs.last(), p2);
        QVERIFY(go::isIdentity(g, 0)); // mixers of neutral nodes: still identity
        g.node(p)->params.setBase(gk::Saturation, 0.5);
        QVERIFY(!go::isIdentity(g, 0));
        g.node(p)->params.setBase(gk::Saturation, 1.0);
        // Removing the extra inputs dissolves the mixer.
        QVERIFY(go::removeNode(g, p2));
        QVERIFY(go::removeNode(g, p));
        QCOMPARE(g.nodes.size(), 1);
        QCOMPARE(g.output, a);
        // Layer mixer: the new node is the top (last) input.
        const quint64 l = go::addLayerNode(g, a);
        QCOMPARE(g.node(g.output)->type, GradeNodeType::Layer);
        QCOMPARE(g.node(g.output)->inputs, (QVector<quint64> {a, l}));
        // Connect refuses cycles and unknown targets; disconnect keeps one mixer input.
        QVERIFY(!go::connect(g, g.output, a, 0));
        QVERIFY(!go::connect(g, a, 999, 0));
        QVERIFY(go::disconnect(g, g.output, 1));
        QVERIFY(!go::disconnect(g, g.output, 0));
        QVERIFY(go::connect(g, l, g.output, 1));
        QVERIFY(go::reaches(g, g.output, l));
        // A disconnected output passes the picture through.
        QVERIFY(go::disconnect(g, 0, 0));
        QVERIFY(go::evaluationOrder(g).empty());
        QVERIFY(go::isIdentity(g, 0));
    }

    void windowsShareTheIdSpace()
    {
        ClipGrade g;
        const quint64 a = go::initialize(g);
        const quint64 w = go::addWindow(g, a, GradeWindowShape::Circle);
        QVERIFY(w > a);
        const quint64 b = go::addSerialNode(g, a);
        QVERIFY(b > w);
        const GradeNode* owner = nullptr;
        QVERIFY(g.window(w, &owner));
        QCOMPARE(owner->id, a);
        QVERIFY(g.node(a)->hasKey());
        for (int i = 1; i < kMaxGradeWindows; ++i)
            QVERIFY(go::addWindow(g, a, GradeWindowShape::Linear));
        QCOMPARE(go::addWindow(g, a, GradeWindowShape::Linear), quint64(0)); // full
        QVERIFY(go::removeWindow(g, w));
        QVERIFY(!go::removeWindow(g, w));
        const quint64 mixer = g.node(g.output)->id;
        Q_UNUSED(mixer);
        QCOMPARE(go::addWindow(g, 999, GradeWindowShape::Circle), quint64(0));
    }

    // ---- Keyframes and edits ----

    void keyframePaths()
    {
        ClipData clip = mediaClip(0, 100);
        clip.grade = richGrade();
        const quint64 first = clip.grade.nodes[0].id;
        const AnimatedValue* v
            = keyframeops::findValue(clip, keyframeops::gradePath(first, gk::GainR));
        QVERIFY(v && v->keys.size() == 2);
        quint64 window = 0;
        for (const GradeNode& n : clip.grade.nodes) {
            if (!n.windows.isEmpty())
                window = n.windows[0].id;
        }
        QVERIFY(keyframeops::findValue(clip, keyframeops::gradeWindowPath(window, wk::CenterX)));
        QVERIFY(!keyframeops::findValue(clip, keyframeops::gradePath(999, gk::GainR)));
        // The keyframe editors list the grade groups.
        const QVector<keyframeops::AnimatedParam> params = keyframeops::animatedParams(clip);
        int grade = 0;
        for (const auto& p : params) {
            if (p.path.startsWith(QLatin1String("grade")))
                ++grade;
        }
        QCOMPARE(grade, 2);
        QVERIFY(keyframeops::allKeyFrames(clip).contains(24));
        QVERIFY(keyframeops::allKeyFrames(clip).contains(10));
    }

    void splitShiftsGradeKeys()
    {
        TimelineData d = timeline();
        ClipData clip = mediaClip(10, 100);
        clip.grade = richGrade();
        const quint64 id = editops::overwriteClip(d, kV1, clip);
        QVERIFY(id);
        QVERIFY(!editops::splitAt(d, 30, editops::unlockedTracks()).isEmpty());
        const TrackData& t = *d.track(kV1);
        QCOMPARE(t.clips.size(), size_t(2));
        const quint64 first = t.clips[1].grade.nodes[0].id;
        const AnimatedValue& v = t.clips[1].grade.node(first)->params.values[gk::GainR];
        QCOMPARE(v.keys.first().frame, int64_t(-20));
        QCOMPARE(
            t.clips[0].grade.node(first)->params.values[gk::GainR].keys.first().frame, int64_t(0));
        // Both halves keep the same grade, so they can be joined again.
        QVERIFY(editops::canJoin(d, t.clips[0].id, t.clips[1].id));
        TimelineData other = d;
        other.track(kV1)->clips[1].grade.nodes[0].params.setBase(gk::Hue, 10.0);
        QVERIFY(!editops::canJoin(other, t.clips[0].id, t.clips[1].id));
    }

    // ---- Serialization ----

    void serializationRoundTrip()
    {
        const ClipGrade g = richGrade();
        const QJsonObject json = serialization::gradeToJson(g);
        QCOMPARE(serialization::gradeFromJson(json), g);
        // Through text as well.
        const QJsonObject text = QJsonDocument::fromJson(QJsonDocument(json).toJson()).object();
        QCOMPARE(serialization::gradeFromJson(text), g);
        // A clip and a timeline carry it; an ungraded clip writes no "grade".
        ClipData clip = mediaClip(0, 10);
        QVERIFY(!serialization::clipToJson(clip).contains(QLatin1String("grade")));
        clip.grade = g;
        QCOMPARE(serialization::clipFromJson(serialization::clipToJson(clip)).grade, g);
        TimelineData tl = timeline();
        QVERIFY(!serialization::timelineToJson(tl).contains(QLatin1String("grade")));
        tl.grade = g;
        TimelineData back;
        QString error;
        QVERIFY2(serialization::timelineFromJson(serialization::timelineToJson(tl), back, &error),
            qPrintable(error));
        QCOMPARE(back.grade, g);
    }

    void damagedJsonIsRepaired()
    {
        const QByteArray text = R"({"output": 7, "nodes": [
            {"id": 1, "type": "corrector", "inputs": [0], "pos": [0, 0],
             "windows": [{"id": 1, "shape": "linear"}, {"id": 0, "shape": "bogus"}]},
            {"id": 1, "type": "corrector", "inputs": [0]},
            {"id": 0, "type": "layer", "inputs": [1]},
            {"id": 3, "type": "corrector", "inputs": [1, 2, 99]},
            {"id": 4, "type": "parallel", "inputs": [3, 42]}]})";
        const ClipGrade g = serialization::gradeFromJson(QJsonDocument::fromJson(text).object());
        QCOMPARE(g.nodes.size(), 3); // duplicate and zero ids dropped
        QCOMPARE(g.output, quint64(0)); // unknown output
        QCOMPARE(g.node(3)->inputs, QVector<quint64> {1}); // a corrector keeps one input
        QCOMPARE(g.node(4)->inputs, (QVector<quint64> {3, 0})); // unknown ids read the source
        // Window ids are made unique within the grade.
        const QVector<GradeWindow>& w = g.node(1)->windows;
        QCOMPARE(w.size(), 2);
        QVERIFY(w[0].id != 1 && w[0].id != 0 && w[1].id != 0 && w[0].id != w[1].id);
        QCOMPARE(w[1].shape, GradeWindowShape::Circle); // unknown shape
        QCOMPARE(g.allocateId(), std::max(w[0].id, w[1].id) + 1);
    }

    void formatVersionAndMigration()
    {
        QCOMPARE(serialization::kFormatVersion, 16);
        QTemporaryDir dir;
        // A format 15 file (a composition, no grade, no gallery) loads unchanged.
        const QByteArray v15 = R"({"formatVersion": 15, "project": {"name": "old"},
            "mediaPool": {"bins": [], "items": []},
            "timelines": [{"uid": 1, "name": "T", "fps": {"num": 25, "den": 1}, "width": 1920,
              "height": 1080, "nextId": 10, "videoTracks": [{"id": 1, "clips": [
                {"id": 2, "kind": "solid", "start": 0, "duration": 100,
                 "composition": {"nodes": [], "mediaOut": 0}}]}],
              "audioTracks": []}]})";
        const QString oldPath = dir.filePath(QStringLiteral("v15.kkproj"));
        QFile o(oldPath);
        QVERIFY(o.open(QIODevice::WriteOnly));
        o.write(v15);
        o.close();
        Project old;
        QString error;
        QVERIFY2(old.load(oldPath, &error), qPrintable(error));
        const TimelineData& d = old.timeline()->data();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(1));
        QVERIFY(d.videoTracks[0].clips[0].grade.isEmpty());
        QVERIFY(d.videoTracks[0].clips[0].composition);
        QVERIFY(d.grade.isEmpty());
        QVERIFY(old.gallery()->stills().isEmpty());
        QVERIFY(!old.isDirty());
        // Saved again: format 16, still no grade or gallery keys (an ungraded project's file
        // only differs by its version number).
        const QString again = dir.filePath(QStringLiteral("again.kkproj"));
        QVERIFY2(old.save(again, &error), qPrintable(error));
        QFile f(again);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray saved = f.readAll();
        const QJsonObject root = QJsonDocument::fromJson(saved).object();
        QCOMPARE(root.value(QLatin1String("formatVersion")).toInt(), 16);
        QVERIFY(!root.contains(QLatin1String("gallery")));
        QVERIFY(!saved.contains("\"grade\""));
    }

    void projectFileRoundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("graded.kkproj"));
        Project p;
        const quint64 uid = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        const quint64 id
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 50));
        p.endEdit();
        QVERIFY(p.setClipGrade(uid, id, richGrade(), QStringLiteral("Grade")));
        ClipGrade tl;
        go::initialize(tl);
        tl.nodes[0].params.setBase(gk::Saturation, 0.5);
        QVERIFY(p.setTimelineGrade(uid, tl, QStringLiteral("Timeline Grade")));
        QImage still(64, 36, QImage::Format_RGB32);
        still.fill(QColor(200, 100, 50));
        p.gallery()->addStill(still, richGrade());
        QString error;
        QVERIFY2(p.save(path, &error), qPrintable(error));
        Project again;
        QVERIFY2(again.load(path, &error), qPrintable(error));
        QCOMPARE(again.clipGrade(again.activeTimelineUid(), id), richGrade());
        QCOMPARE(again.timelineGrade(again.activeTimelineUid()), tl);
        QCOMPARE(again.gallery()->stills().size(), 1);
        const GalleryStill& s = again.gallery()->stills()[0];
        QCOMPARE(s.grade, richGrade());
        QCOMPARE(s.image.size(), QSize(64, 36));
        QVERIFY(std::abs(QColor(s.image.pixel(10, 10)).red() - 200) < 8); // JPEG
        QCOMPARE(s.name, QStringLiteral("1.1"));
    }

    // ---- Undo ----

    void projectUndo()
    {
        Project p;
        const quint64 uid = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        const quint64 a
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 50));
        const quint64 b
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(50, 50));
        p.endEdit();
        const int steps = p.undoStack()->count();
        QVERIFY(p.clipGrade(uid, a).isEmpty());

        ClipGrade g;
        const quint64 node = go::initialize(g);
        QVERIFY(p.setClipGrade(uid, a, g, QStringLiteral("Add Serial Node")));
        QCOMPARE(p.undoStack()->count(), steps + 1);
        QCOMPARE(p.undoStack()->undoText(), QStringLiteral("Add Serial Node"));
        // The same value again makes no step.
        QVERIFY(p.setClipGrade(uid, a, g, QStringLiteral("Nothing")));
        QCOMPARE(p.undoStack()->count(), steps + 1);
        // A wheel drag: many calls inside one open edit are one step.
        p.beginEdit(QStringLiteral("Gain"));
        for (const double v : {1.1, 1.2, 1.3}) {
            g.node(node)->params.setBase(gk::GainY, v);
            QVERIFY(p.setClipGrade(uid, a, g, QStringLiteral("Gain")));
        }
        p.endEdit();
        QCOMPARE(p.undoStack()->count(), steps + 2);
        QCOMPARE(p.clipGrade(uid, a).node(node)->value(gk::GainY, 0), 1.3);
        // Copy the grade to both clips in one step.
        QCOMPARE(p.setClipGrades(uid, {a, b, 999}, g, QStringLiteral("Apply Grade")), 1);
        QCOMPARE(p.undoStack()->count(), steps + 3);
        QCOMPARE(p.clipGrade(uid, b), g);
        // The timeline grade.
        QVERIFY(p.setTimelineGrade(uid, g, QStringLiteral("Timeline Grade")));
        QCOMPARE(p.undoStack()->count(), steps + 4);
        p.undo();
        QVERIFY(p.timelineGrade(uid).isEmpty());
        p.undo();
        QVERIFY(p.clipGrade(uid, b).isEmpty());
        p.undo();
        QCOMPARE(p.clipGrade(uid, a).node(node)->value(gk::GainY, 0), 1.0);
        p.undo();
        QVERIFY(p.clipGrade(uid, a).isEmpty());
        p.redo();
        QCOMPARE(p.clipGrade(uid, a), [] {
            ClipGrade x;
            go::initialize(x);
            return x;
        }());
        // Locked tracks and unknown clips / timelines are refused.
        p.beginEdit(QStringLiteral("Lock"));
        p.timeline()->mutableData().track(kV1)->locked = true;
        p.endEdit();
        QVERIFY(!p.setClipGrade(uid, a, ClipGrade {}, QStringLiteral("x")));
        QVERIFY(!p.setClipGrade(uid, 999, g, QStringLiteral("x")));
        QVERIFY(!p.setTimelineGrade(9999, g, QStringLiteral("x")));
    }

    void inactiveTimelineUndo()
    {
        Project p;
        const quint64 first = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        const quint64 id
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 50));
        p.endEdit();
        const quint64 second = p.createTimeline(QStringLiteral("Other"));
        QVERIFY(p.setActiveTimeline(second));
        ClipGrade g;
        go::initialize(g);
        QVERIFY(p.setClipGrade(first, id, g, QStringLiteral("Grade")));
        QVERIFY(p.setTimelineGrade(first, g, QStringLiteral("Timeline Grade")));
        QCOMPARE(p.clipGrade(first, id), g);
        QCOMPARE(p.activeTimelineUid(), second);
        p.undo();
        p.undo();
        QVERIFY(p.clipGrade(first, id).isEmpty());
        QVERIFY(p.timelineGrade(first).isEmpty());
    }

    // ---- Gallery ----

    void gallery()
    {
        Project p;
        QVERIFY(!p.isDirty());
        GradeGallery* gallery = p.gallery();
        QSignalSpy changed(gallery, &GradeGallery::changed);
        QImage big(1280, 720, QImage::Format_RGB32);
        big.fill(Qt::red);
        const quint64 a = gallery->addStill(big, richGrade());
        const quint64 b = gallery->addStill(big, ClipGrade {});
        QCOMPARE(changed.count(), 2);
        QVERIFY(p.isDirty()); // not undoable, but the project has to be saved
        QCOMPARE(gallery->still(a)->image.width(), kGalleryImageWidth);
        QCOMPARE(gallery->still(b)->name, QStringLiteral("1.2"));
        QVERIFY(gallery->renameStill(b, QStringLiteral("Night")));
        QCOMPARE(gallery->still(b)->name, QStringLiteral("Night"));
        GradeGallery copy;
        copy.fromJson(gallery->toJson());
        QCOMPARE(copy.stills().size(), 2);
        QCOMPARE(copy.still(a)->grade, richGrade());
        QVERIFY(gallery->removeStill(a));
        QVERIFY(!gallery->removeStill(a));
        p.newProject();
        QVERIFY(gallery->stills().isEmpty());
        QVERIFY(!p.isDirty());
    }
};

QTEST_GUILESS_MAIN(TestGrade)
#include "test_grade.moc"
