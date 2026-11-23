// SPDX-License-Identifier: GPL-3.0-or-later
// Color Match palette, core part (docs/color-page.md, spec 153): Shot Match from image
// statistics, the colour chart (reference colours, patch measurement through a tilted quad, the
// tone + matrix fit that recovers a known transform), and the result as ordinary grade
// parameters (the CPU grade path gives what the maths says).
#include "core/ColorMatch.h"
#include "core/Grade.h"
#include "core/GradeMath.h"
#include "render/GradeEval.h"

#include <QImage>
#include <QTest>

#include <cmath>
#include <random>
#include <vector>

using namespace kk;
namespace cm = kk::colormatch;
namespace go = kk::gradeops;
namespace gk = kk::gradekeys;

namespace {

/// A picture of random colours: each channel uniform in [lo, hi] with a different cast.
std::vector<float> noisePicture(size_t pixels, const float lo[3], const float hi[3], unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    std::vector<float> v(pixels * 3);
    for (size_t i = 0; i < pixels; ++i) {
        for (int ch = 0; ch < 3; ++ch)
            v[i * 3 + size_t(ch)] = lo[ch] + (hi[ch] - lo[ch]) * d(rng);
    }
    return v;
}

void applyShot(const cm::ShotMatch& m, std::vector<float>& v)
{
    for (size_t i = 0; i < v.size(); ++i) {
        const int ch = int(i % 3);
        v[i] = m.gain[ch] * (v[i] + m.offset[ch]);
    }
}

double meanOf(const std::vector<float>& v, int ch)
{
    double s = 0;
    size_t n = 0;
    for (size_t i = size_t(ch); i < v.size(); i += 3, ++n)
        s += double(v[i]);
    return s / double(n);
}

/// The inverse of a ChartMatch's tone and matrix (what the camera did to the chart).
void invertMatch(const cm::ChartMatch& m, const float in[3], float out[3])
{
    // Matrix inverse (3 x 3, row-major).
    const float* a = m.mixer;
    const float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6])
        + a[2] * (a[3] * a[7] - a[4] * a[6]);
    const float inv[9] = {(a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det,
        (a[1] * a[5] - a[2] * a[4]) / det, (a[5] * a[6] - a[3] * a[8]) / det,
        (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
        (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det,
        (a[0] * a[4] - a[1] * a[3]) / det};
    float y[3];
    for (int r = 0; r < 3; ++r)
        y[r] = inv[r * 3] * in[0] + inv[r * 3 + 1] * in[1] + inv[r * 3 + 2] * in[2];
    for (int ch = 0; ch < 3; ++ch) {
        const float e = std::exp2(-2.0f * m.gamma[ch]);
        const float v = std::pow(std::max(y[ch], 0.0f), 1.0f / e);
        out[ch] = (v / m.gain[ch] - m.lift[ch]) / (1.0f - m.lift[ch]);
    }
}

} // namespace

class TestColorMatch : public QObject
{
    Q_OBJECT
private slots:
    void shotStatsOfKnownPicture()
    {
        const float lo[3] = {0.1f, 0.2f, 0.3f}, hi[3] = {0.5f, 0.6f, 0.9f};
        const auto v = noisePicture(20000, lo, hi, 1);
        const cm::ShotStats s = cm::shotStats(v.data(), v.size() / 3);
        QVERIFY(s.valid);
        for (int ch = 0; ch < 3; ++ch) {
            QVERIFY(std::abs(s.mid[ch] - (lo[ch] + hi[ch]) / 2) < 0.01f);
            QVERIFY(std::abs(s.low[ch] - (lo[ch] + 0.05f * (hi[ch] - lo[ch]))) < 0.01f);
            QVERIFY(std::abs(s.high[ch] - (lo[ch] + 0.95f * (hi[ch] - lo[ch]))) < 0.01f);
        }
        QVERIFY(!cm::shotStats(v.data(), 5).valid);
    }

    void shotMatchMovesTowardTarget()
    {
        // A dull blue shot and a warm, contrasty target.
        const float slo[3] = {0.05f, 0.1f, 0.2f}, shi[3] = {0.45f, 0.55f, 0.8f};
        const float tlo[3] = {0.1f, 0.05f, 0.02f}, thi[3] = {0.95f, 0.8f, 0.6f};
        auto source = noisePicture(20000, slo, shi, 2);
        const auto target = noisePicture(20000, tlo, thi, 3);
        const cm::ShotStats ts = cm::shotStats(target.data(), target.size() / 3);
        const cm::ShotMatch m = cm::shotMatch(cm::shotStats(source.data(), source.size() / 3), ts);
        QVERIFY(m.valid);
        double before[3], after[3];
        for (int ch = 0; ch < 3; ++ch)
            before[ch] = std::abs(meanOf(source, ch) - meanOf(target, ch));
        applyShot(m, source);
        for (int ch = 0; ch < 3; ++ch) {
            after[ch] = std::abs(meanOf(source, ch) - meanOf(target, ch));
            QVERIFY2(after[ch] < before[ch] * 0.2, "the mean moves to the target's");
        }
        // The spread follows too.
        const cm::ShotStats done = cm::shotStats(source.data(), source.size() / 3);
        for (int ch = 0; ch < 3; ++ch)
            QVERIFY(std::abs((done.high[ch] - done.low[ch]) - (ts.high[ch] - ts.low[ch])) < 0.06f);
    }

    void shotMatchStrengthAndLimits()
    {
        const float slo[3] = {0.2f, 0.2f, 0.2f}, shi[3] = {0.4f, 0.4f, 0.4f};
        const float tlo[3] = {0.0f, 0.0f, 0.0f}, thi[3] = {1.0f, 1.0f, 1.0f};
        const auto s = noisePicture(5000, slo, shi, 4);
        const auto t = noisePicture(5000, tlo, thi, 5);
        const cm::ShotStats ss = cm::shotStats(s.data(), s.size() / 3);
        const cm::ShotStats ts = cm::shotStats(t.data(), t.size() / 3);
        const cm::ShotMatch full = cm::shotMatch(ss, ts);
        QCOMPARE(full.gain[0], cm::kShotGainMax); // limited
        const cm::ShotMatch none = cm::shotMatch(ss, ts, 0.0);
        for (int ch = 0; ch < 3; ++ch) {
            QVERIFY(std::abs(none.gain[ch] - 1.0f) < 1e-6f);
            QVERIFY(std::abs(none.offset[ch]) < 1e-6f);
        }
        const cm::ShotMatch half = cm::shotMatch(ss, ts, 0.5);
        QVERIFY(half.gain[0] > 1.0f && half.gain[0] < full.gain[0]);
        QVERIFY(!cm::shotMatch(cm::ShotStats {}, ts).valid);
    }

    void shotMatchComposes()
    {
        cm::ShotMatch a, b;
        a.valid = b.valid = true;
        const float ga[3] = {1.2f, 0.9f, 1.5f}, oa[3] = {0.05f, -0.1f, 0.02f};
        const float gb[3] = {0.8f, 1.1f, 1.3f}, ob[3] = {-0.03f, 0.04f, 0.1f};
        for (int ch = 0; ch < 3; ++ch) {
            a.gain[ch] = ga[ch];
            a.offset[ch] = oa[ch];
            b.gain[ch] = gb[ch];
            b.offset[ch] = ob[ch];
        }
        const cm::ShotMatch c = cm::composeShotMatch(a, b);
        for (int ch = 0; ch < 3; ++ch) {
            const float x = 0.37f;
            const float two = b.gain[ch] * (a.gain[ch] * (x + a.offset[ch]) + b.offset[ch]);
            QVERIFY(std::abs(two - c.gain[ch] * (x + c.offset[ch])) < 1e-5f);
        }
    }

    void shotMatchParamsAreGradeKeys()
    {
        cm::ShotMatch m;
        m.valid = true;
        m.gain[1] = 1.3f;
        m.offset[2] = -0.07f;
        double gain = 0, offset = 0;
        for (const auto& [k, v] : cm::shotMatchParams(m)) {
            if (k == gk::GainG)
                gain = v;
            if (k == gk::OffsetB)
                offset = v;
        }
        QVERIFY(std::abs(gain - 1.3) < 1e-6);
        QVERIFY(std::abs(offset + 0.07) < 1e-6);
    }

    void shotMatchIntoGrade()
    {
        cm::ShotMatch a;
        a.valid = true;
        a.gain[0] = 1.2f;
        a.offset[1] = 0.05f;
        ClipGrade g;
        const quint64 id = cm::applyShotMatch(g, a);
        QVERIFY(id != 0);
        QCOMPARE(g.nodes.size(), 1);
        QCOMPARE(g.node(id)->label, cm::kShotMatchLabel);
        QVERIFY(std::abs(g.node(id)->value(gk::GainR, 0) - 1.2) < 1e-6);
        // A second match joins the same node and composes exactly.
        cm::ShotMatch b = a;
        const quint64 again = cm::applyShotMatch(g, b);
        QCOMPARE(again, id);
        QCOMPARE(g.nodes.size(), 1);
        QVERIFY(std::abs(g.node(id)->value(gk::GainR, 0) - 1.44) < 1e-5);
        QVERIFY(std::abs(g.node(id)->value(gk::OffsetG, 0) - 0.1) < 1e-5);
        // A grade made by hand is kept: the match goes into a new node after it.
        ClipGrade hand;
        const quint64 first = go::initialize(hand);
        hand.node(first)->params.setBase(gk::Saturation, 1.4);
        const quint64 added = cm::applyShotMatch(hand, a);
        QVERIFY(added != 0 && added != first);
        QCOMPARE(hand.nodes.size(), 2);
        QCOMPARE(hand.output, added);
        QVERIFY(std::abs(hand.node(first)->value(gk::Saturation, 0) - 1.4) < 1e-9);
        QCOMPARE(cm::applyShotMatch(hand, cm::ShotMatch {}), quint64(0));
    }

    void chartToNodeResetsTheBalance()
    {
        ClipGrade g;
        const quint64 id = go::initialize(g);
        GradeNode* n = g.node(id);
        n->params.setBase(gk::OffsetR, 0.1);
        n->params.setBase(gk::GainY, 1.5);
        n->params.setBase(gk::MixerMono, 1.0);
        n->params.setBase(gk::Saturation, 1.3);
        cm::ChartMatch m;
        m.valid = true;
        m.gain[1] = 1.1f;
        cm::applyChartToNode(*n, m);
        QCOMPARE(n->value(gk::OffsetR, 0), 0.0);
        QCOMPARE(n->value(gk::GainY, 0), 1.0);
        QCOMPARE(n->value(gk::MixerMono, 0), 0.0);
        QVERIFY(std::abs(n->value(gk::GainG, 0) - 1.1) < 1e-6);
        QCOMPARE(n->value(gk::Saturation, 0), 1.3); // other values stay
    }

    void gammaRoundTrip()
    {
        for (int g = 0; g <= int(cm::Gamma::Rec709); ++g) {
            for (const float x : {0.0f, 0.004f, 0.02f, 0.18f, 0.5f, 1.0f}) {
                const float e = cm::gammaEncode(cm::Gamma(g), x);
                QVERIFY(std::abs(cm::gammaDecode(cm::Gamma(g), e) - x) < 1e-4f);
            }
        }
        QVERIFY(std::abs(cm::gammaEncode(cm::Gamma::Srgb, 0.18f) - 0.4614f) < 1e-3f);
    }

    void idsRoundTrip()
    {
        for (int g = 0; g <= int(cm::Gamma::Rec709); ++g)
            QCOMPARE(int(cm::gammaFromId(cm::gammaId(cm::Gamma(g)))), g);
        for (int s = 0; s <= int(cm::ColorSpace::Rec2020); ++s)
            QCOMPARE(int(cm::colorSpaceFromId(cm::colorSpaceId(cm::ColorSpace(s)))), s);
        for (int w = 0; w <= int(cm::WhitePoint::Dci); ++w)
            QCOMPARE(int(cm::whitePointFromId(cm::whitePointId(cm::WhitePoint(w)))), w);
        QCOMPARE(int(cm::chartFromId(cm::chartId(cm::ChartType::ColorCheckerPassport))),
            int(cm::ChartType::ColorCheckerPassport));
        QCOMPARE(cm::chartPatchName(cm::ChartType::ColorCheckerClassic, 18), QStringLiteral("White"));
    }

    void targetColorsNeutralInEverySpace()
    {
        for (int s = 0; s <= int(cm::ColorSpace::Rec2020); ++s) {
            for (int w = 0; w <= int(cm::WhitePoint::Dci); ++w) {
                cm::ChartTarget t;
                t.space = cm::ColorSpace(s);
                t.white = cm::WhitePoint(w);
                const auto c = cm::chartTargetColors(cm::ChartType::ColorCheckerClassic, t);
                for (int i = cm::kChartFirstNeutral; i < cm::kChartPatches; ++i) {
                    QVERIFY(std::abs(c[size_t(i)][0] - c[size_t(i)][1]) < 0.01f);
                    QVERIFY(std::abs(c[size_t(i)][1] - c[size_t(i)][2]) < 0.01f);
                }
                // Neutral patches get darker down the row.
                QVERIFY(c[18][1] > c[19][1] && c[19][1] > c[22][1] && c[22][1] > c[23][1]);
            }
        }
        // Rec.709 + sRGB gamma keeps the published sRGB values.
        cm::ChartTarget t;
        t.targetGamma = cm::Gamma::Srgb;
        const auto c = cm::chartTargetColors(cm::ChartType::ColorCheckerClassic, t);
        QVERIFY(std::abs(c[0][0] - 115.0f / 255.0f) < 0.003f);
        QVERIFY(std::abs(c[14][2] - 60.0f / 255.0f) < 0.003f);
        // A wider gamut gives a less saturated number for the same colour.
        t.space = cm::ColorSpace::Rec2020;
        const auto w = cm::chartTargetColors(cm::ChartType::ColorCheckerClassic, t);
        QVERIFY(w[14][0] - w[14][2] < c[14][0] - c[14][2]); // red patch
    }

    void measureTiltedChart()
    {
        // Paint the chart through a skewed quad, then measure it again.
        constexpr int W = 480, H = 270;
        cm::ChartTarget target;
        const auto want = cm::chartTargetColors(cm::ChartType::ColorCheckerClassic, target);
        std::vector<float> img(size_t(W) * H * 3, 0.1f);
        cm::ChartQuad quad;
        quad.corner = {QPointF(0.22, 0.28), QPointF(0.74, 0.22), QPointF(0.78, 0.74),
            QPointF(0.25, 0.70)};
        for (int p = 0; p < cm::kChartPatches; ++p) {
            const int col = p % cm::kChartColumns, row = p / cm::kChartColumns;
            for (int j = 0; j < 80; ++j) {
                for (int i = 0; i < 80; ++i) {
                    const QPointF pt = cm::chartPoint(quad, (col + (i + 0.5) / 80) / cm::kChartColumns,
                        (row + (j + 0.5) / 80) / cm::kChartRows);
                    const int x = int(pt.x() * W), y = int(pt.y() * H);
                    for (int c = 0; c < 3; ++c)
                        img[(size_t(y) * W + size_t(x)) * 3 + size_t(c)] = want[size_t(p)][size_t(c)];
                }
            }
        }
        cm::PatchColors got {};
        QVERIFY(cm::measureChart(img.data(), W, H, quad, got));
        for (int p = 0; p < cm::kChartPatches; ++p) {
            for (int c = 0; c < 3; ++c)
                QVERIFY2(std::abs(got[size_t(p)][size_t(c)] - want[size_t(p)][size_t(c)]) < 0.02f,
                    qPrintable(QStringLiteral("patch %1").arg(p)));
        }
        // A chart that leaves the picture, or a bow-tie, cannot be measured.
        cm::ChartQuad out = quad;
        out.corner[1] = QPointF(1.4, 0.2);
        QVERIFY(!cm::measureChart(img.data(), W, H, out, got));
        cm::ChartQuad bow = quad;
        std::swap(bow.corner[2], bow.corner[3]);
        QVERIFY(!cm::measureChart(img.data(), W, H, bow, got));
    }

    void chartQuadDefaults()
    {
        const cm::ChartQuad q = cm::defaultChartQuad(16.0 / 9.0);
        QVERIFY(std::abs(q.corner[0].x() - 0.3) < 1e-9);
        // 3:2 on screen: width 0.4 * 16, height 0.4 / 1.5 * 9 in the same units.
        const double pw = (q.corner[1].x() - q.corner[0].x()) * 16.0;
        const double ph = (q.corner[3].y() - q.corner[0].y()) * 9.0;
        QVERIFY(std::abs(pw / ph - 1.5) < 1e-6);
        const QPointF mid = cm::chartPoint(q, 0.5, 0.5);
        QVERIFY(std::abs(mid.x() - 0.5) < 1e-9 && std::abs(mid.y() - 0.5) < 1e-9);
    }

    void chartRecoversKnownTransform()
    {
        // What a made-up camera did to the chart: the inverse of a known grade.
        cm::ChartMatch truth;
        truth.lift[0] = 0.02f;
        truth.lift[1] = 0.01f;
        truth.lift[2] = 0.03f;
        truth.gamma[0] = -0.05f;
        truth.gamma[1] = 0.0f;
        truth.gamma[2] = 0.06f;
        truth.gain[0] = 0.9f;
        truth.gain[1] = 1.05f;
        truth.gain[2] = 1.2f;
        const float mx[9] = {1.12f, -0.08f, -0.04f, -0.05f, 1.1f, -0.05f, -0.02f, -0.1f, 1.12f};
        std::copy(mx, mx + 9, truth.mixer);
        cm::ChartTarget target;
        const auto want = cm::chartTargetColors(cm::ChartType::ColorCheckerClassic, target);
        cm::PatchColors measured {};
        for (int p = 0; p < cm::kChartPatches; ++p) {
            invertMatch(truth, want[size_t(p)].data(), measured[size_t(p)].data());
            for (int c = 0; c < 3; ++c)
                QVERIFY(measured[size_t(p)][size_t(c)] > -0.2f && measured[size_t(p)][size_t(c)] < 1.3f);
        }
        const cm::ChartMatch got =
            cm::solveChart(measured, cm::ChartType::ColorCheckerClassic, target);
        QVERIFY(got.valid);
        QVERIFY(got.errorBefore > 0.05);
        QVERIFY2(got.errorAfter < 0.02, qPrintable(QString::number(got.errorAfter)));
        // The neutral axis is right to within a level or two.
        for (int p = cm::kChartFirstNeutral; p < cm::kChartPatches; ++p) {
            float c[3] = {measured[size_t(p)][0], measured[size_t(p)][1], measured[size_t(p)][2]};
            cm::applyChartMatch(got, c);
            for (int ch = 0; ch < 3; ++ch)
                QVERIFY(std::abs(c[ch] - want[size_t(p)][size_t(ch)]) < 0.015f);
        }
    }

    void chartFlatOrBadInput()
    {
        cm::PatchColors flat {};
        for (auto& p : flat)
            p = {0.4f, 0.4f, 0.4f};
        const cm::ChartMatch m = cm::solveChart(flat, cm::ChartType::ColorCheckerClassic, {});
        QVERIFY(!m.valid);
    }

    void chartNeedsOnlyTheRightGammaStart()
    {
        // A linear-light capture of the chart: the tone fit must find the gamma by itself.
        cm::ChartTarget target;
        target.sourceGamma = cm::Gamma::Linear;
        target.targetGamma = cm::Gamma::Gamma22;
        const auto want = cm::chartTargetColors(cm::ChartType::ColorCheckerClassic, target);
        cm::PatchColors measured {};
        for (int p = 0; p < cm::kChartPatches; ++p) {
            for (int c = 0; c < 3; ++c)
                measured[size_t(p)][size_t(c)] = cm::gammaDecode(cm::Gamma::Gamma22, want[size_t(p)][size_t(c)]);
        }
        const cm::ChartMatch got =
            cm::solveChart(measured, cm::ChartType::ColorCheckerClassic, target);
        QVERIFY(got.valid);
        QVERIFY2(got.errorAfter < 0.01, qPrintable(QString::number(got.errorAfter)));
        QVERIFY(got.errorAfter < got.errorBefore * 0.2);
    }

    void chartParamsRunThroughTheGrade()
    {
        cm::ChartMatch m;
        m.valid = true;
        m.lift[0] = 0.02f;
        m.gamma[1] = 0.08f;
        m.gain[2] = 1.15f;
        const float mx[9] = {1.1f, -0.05f, -0.05f, 0.0f, 1.0f, 0.0f, -0.1f, -0.05f, 1.15f};
        std::copy(mx, mx + 9, m.mixer);
        ClipGrade g;
        const quint64 id = go::initialize(g);
        for (const auto& [k, v] : cm::chartMatchParams(m))
            g.node(id)->params.setBase(k, v);
        QImage img(8, 8, QImage::Format_ARGB32);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x)
                img.setPixel(x, y, qRgb(30 + x * 25, 200 - y * 20, 40 + (x + y) * 12));
        }
        QImage graded = img;
        render::applyGrade(g, 0, graded);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const QRgb p = img.pixel(x, y);
                float c[3] = {qRed(p) / 255.0f, qGreen(p) / 255.0f, qBlue(p) / 255.0f};
                cm::applyChartMatch(m, c);
                const QRgb q = graded.pixel(x, y);
                QVERIFY(std::abs(std::clamp(c[0], 0.0f, 1.0f) * 255.0f - qRed(q)) <= 2.5f);
                QVERIFY(std::abs(std::clamp(c[1], 0.0f, 1.0f) * 255.0f - qGreen(q)) <= 2.5f);
                QVERIFY(std::abs(std::clamp(c[2], 0.0f, 1.0f) * 255.0f - qBlue(q)) <= 2.5f);
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestColorMatch)
#include "test_colormatch.moc"
