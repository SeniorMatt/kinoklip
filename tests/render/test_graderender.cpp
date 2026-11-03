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
#include "render/GradeEval.h"
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
            p.window(poly)->points = {QPointF(-0.5, -0.4), QPointF(0.4, -0.5), QPointF(0.1, 0.0),
                QPointF(0.5, 0.5), QPointF(-0.3, 0.3)};
            p.window(poly)->params.setBase(wk::Width, 0.8);
            p.window(poly)->params.setBase(wk::Height, 0.9);
            p.window(poly)->params.setBase(wk::SoftOut, 0.08);
            const quint64 grad = go::addWindow(p, m, GradeWindowShape::Gradient);
            p.window(grad)->params.setBase(wk::Rotation, -60.0);
            p.window(grad)->invert = true;
            p.window(grad)->params.setBase(wk::Opacity, 0.7);
            out.append({QStringLiteral("polygon_gradient"), p});
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
        return out;
    }

private slots:
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
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestGradeRender>(argc, argv);
}

#include "test_graderender.moc"
