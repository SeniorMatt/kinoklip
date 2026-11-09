// SPDX-License-Identifier: GPL-3.0-or-later
// The video transitions of the Effects library folders (spec 53): the registry rows (folder and
// parameters of every transition), one table-driven render test for all of them (the first frame
// is mostly the outgoing picture, the last mostly the incoming one, the middle differs from both
// and, where it should, from a plain dissolve), the CPU fallback (a plain dissolve), saving and
// loading, and Smooth Cut on synthetic frames. GPU cases skip without OpenGL.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Serialization.h"
#include "render/Compositor.h"

using namespace kk;
using namespace kk::test;

namespace {

ClipData generatorClip(TimelineData& tl, const QString& id, int64_t start, int64_t duration)
{
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Generator;
    c.generator = id;
    c.title = EffectRegistry::instance().defaultParams(id);
    c.start = start;
    c.duration = duration;
    return c;
}

/// Mean absolute difference of the colour channels of two images, 0..255.
double meanDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    double sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q))
                + std::abs(qBlue(p) - qBlue(q));
        }
    }
    return sum / (3.0 * a.width() * a.height());
}

/// Colour bars (outgoing) and a four colour gradient (incoming), both with plenty of structure.
/// The transition sits on the incoming clip: window [38, 63), progress 0 at frame 38, 0.5 at 50.
constexpr int64_t kStart = 38, kMiddle = 50, kEnd = 62;

TimelineData transitionTimeline(const TransitionData& transition)
{
    TimelineData tl = emptyTimeline();
    addClip(tl, TrackType::Video, 0, generatorClip(tl, generators::ColorBars, 0, 50));
    ClipData b = generatorClip(tl, generators::FourColorGradient, 50, 50);
    b.transitionIn = transition;
    addClip(tl, TrackType::Video, 0, b);
    return tl;
}

TimelineData soloTimeline(const QString& generator)
{
    TimelineData tl = emptyTimeline();
    addClip(tl, TrackType::Video, 0, generatorClip(tl, generator, 0, 100));
    return tl;
}

/// Transitions as the registry creates them (default parameters), then one parameter changed.
TransitionData makeTransition(const QString& id, const QString& key, const QVariant& value)
{
    TransitionData t = EffectRegistry::instance().createTransition(id, 25);
    if (!key.isEmpty()) {
        if (value.typeId() == QMetaType::Double)
            t.params.setBase(key, value.toDouble());
        else
            t.params.setStatic(key, value);
    }
    return t;
}

} // namespace

class TestTransitions : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    QImage render(Compositor& c, const TimelineData& tl, int64_t frame, QSize size = {})
    {
        return c.renderImage(snap(tl), frame, size, Quality::Full);
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
    }
    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    // ---- Registry ----

    void registry_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("group");
        QTest::addColumn<QString>("keys"); ///< parameter keys in order, comma separated
        const QString edge = QStringLiteral("color,border,feather,reverse");
        const QString shape = QStringLiteral("offsetX,offsetY,") + edge;
        const QString ratios = QStringLiteral("startRatio,endRatio");
        auto row = [](const char* id, const char* group, const QString& keys) {
            QTest::newRow(id) << QString::fromLatin1(id) << QString::fromLatin1(group) << keys;
        };
        row("cross_dissolve", "dissolve", {});
        row("dip_to_color", "dissolve", "color");
        row("additive_dissolve", "dissolve", ratios);
        row("blur_dissolve", "dissolve", ratios + ",blurH,blurV");
        row("non_additive_dissolve", "dissolve", ratios);
        row("smooth_cut", "dissolve", {});
        row("arrow_iris", "iris", "preset,aspect,offsetX,offsetY,rotation," + edge);
        row("cross_iris", "iris", shape);
        row("diamond_iris", "iris", shape);
        row("eye_iris", "iris", shape);
        row("hexagon_iris", "iris", "preset,aspect,offsetX,offsetY,rotation," + edge);
        row("oval_iris", "iris", "aspect," + shape);
        row("pentagon_iris", "iris", "preset,aspect,offsetX,offsetY,rotation," + edge);
        row("square_iris", "iris", "preset,aspect,offsetX,offsetY,rotation," + edge);
        row("triangle_iris", "iris", "preset,aspect,offsetX,offsetY,rotation," + edge);
        row("box_shape", "shape", "mode," + edge);
        row("heart_shape", "shape", "aspect,offsetX,offsetY,rotation," + edge);
        row("star_shape", "shape", "preset,aspect,offsetX,offsetY,rotation," + edge);
        row("triangle_left_shape", "shape", edge);
        row("triangle_right_shape", "shape", edge);
        row("wipe", "wipe", "direction,softness");
        row("band_wipe", "wipe", "preset,repeat," + edge);
        row("center_wipe", "wipe", "angle," + edge);
        row("clock_wipe", "wipe", "angle,clockwise,color,border,feather");
        row("radial_wipe", "wipe", edge);
        row("spiral_wipe", "wipe", edge);
        row("venetian_blind_wipe", "wipe", "repeat,angle," + edge);
        row("x_wipe", "wipe", edge);
        const QString place = QStringLiteral("offsetX,offsetY");
        row("brightness_flash", "fusion_transitions", "brightness");
        row("zoom_in", "fusion_transitions", "zoom," + place);
        row("zoom_in_out", "fusion_transitions", "zoom," + place);
        row("crash_zoom", "fusion_transitions", "zoom," + place);
        row("rotate", "fusion_transitions", {});
        row("rotate_90", "fusion_transitions", {});
        row("noise_dissolve", "fusion_transitions", "size,softness");
        row("glitch", "fusion_transitions", "strength");
        row("camera_shake", "fusion_transitions", "speed,strength");
        row("circles", "fusion_transitions", "count," + place);
        row("flip_3d", "fusion_transitions", {});
        row("slice_push", "fusion_transitions", "count");
        row("warp", "fusion_transitions", "strength," + place);
        row("drop_warp", "fusion_transitions", "strength," + place);
        row("tunnel_of_light", "fusion_transitions", "glow,size," + place);
        row("burn_away", "resolve_fx_transitions", "angle,raggedness,glow,char");
        row("push", "motion", "direction");
        row("barn_door", "motion", "preset,reverse,motionBlur");
        row("slide", "motion", "direction,mode,motionBlur");
        row("split", "motion", "preset,reverse,motionBlur");
    }
    void registry()
    {
        QFETCH(QString, id);
        QFETCH(QString, group);
        QFETCH(QString, keys);
        const EffectDescriptor* d = EffectRegistry::instance().find(id);
        QVERIFY2(d, qPrintable(id));
        QCOMPARE(d->category, EffectCategory::Transition);
        QCOMPARE(d->group, group);
        QStringList have;
        for (const ParamDescriptor& p : d->params)
            have.append(p.key);
        QCOMPARE(have.join(QLatin1Char(',')), keys);
    }

    void foldersOfTheTransitions()
    {
        const QStringList groups = EffectRegistry::instance().groups(EffectCategory::Transition);
        QCOMPARE(groups,
            QStringList({QStringLiteral("dissolve"), QStringLiteral("fusion_transitions"),
                QStringLiteral("iris"), QStringLiteral("motion"),
                QStringLiteral("resolve_fx_transitions"), QStringLiteral("shape"),
                QStringLiteral("wipe")}));
        QCOMPARE(EffectRegistry::groupName(QStringLiteral("iris")), QStringLiteral("Iris"));
    }

    // ---- Rendering ----

    void renders_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("key");
        QTest::addColumn<QVariant>("value");
        QTest::addColumn<bool>("differsFromDissolve");
        auto row = [](const char* name, const char* id, const char* key = "",
                       const QVariant& value = {}, bool differs = true) {
            QTest::newRow(name) << QString::fromLatin1(id) << QString::fromLatin1(key) << value
                                << differs;
        };
        row("additive_dissolve", "additive_dissolve");
        row("non_additive_dissolve", "non_additive_dissolve");
        row("blur_dissolve", "blur_dissolve");
        row("blur_dissolve ratios", "blur_dissolve", "startRatio", 20.0);
        // Two still pictures: the flow finds no motion and the morph is a dissolve.
        row("smooth_cut", "smooth_cut", "", {}, false);
        row("arrow_iris", "arrow_iris");
        row("arrow_iris down", "arrow_iris", "preset", QStringLiteral("down"));
        row("cross_iris", "cross_iris");
        row("diamond_iris", "diamond_iris");
        row("eye_iris", "eye_iris");
        row("hexagon_iris", "hexagon_iris");
        row("hexagon_iris vertical", "hexagon_iris", "preset", QStringLiteral("hexagon_vertical"));
        row("oval_iris", "oval_iris");
        row("pentagon_iris", "pentagon_iris");
        row("square_iris", "square_iris");
        row("square_iris point", "square_iris", "preset", QStringLiteral("point"));
        row("triangle_iris", "triangle_iris");
        row("triangle_iris right", "triangle_iris", "preset", QStringLiteral("right"));
        row("triangle_iris offset", "triangle_iris", "offsetX", 40.0);
        row("triangle_iris reverse", "triangle_iris", "reverse", true);
        row("triangle_iris border", "triangle_iris", "border", 12.0);
        row("triangle_iris feather", "triangle_iris", "feather", 40.0);
        row("box_shape", "box_shape");
        row("box_shape bottom", "box_shape", "mode", QStringLiteral("bottom_center"));
        row("heart_shape", "heart_shape");
        row("star_shape", "star_shape");
        row("star_shape 4", "star_shape", "preset", QStringLiteral("4"));
        row("star_shape 6", "star_shape", "preset", QStringLiteral("6"));
        row("triangle_left_shape", "triangle_left_shape");
        row("triangle_right_shape", "triangle_right_shape");
        row("band_wipe", "band_wipe");
        row("band_wipe vertical", "band_wipe", "preset", QStringLiteral("vertical"));
        row("band_wipe bilinear", "band_wipe", "preset", QStringLiteral("horizontal_bilinear"));
        row("center_wipe", "center_wipe");
        row("center_wipe angled", "center_wipe", "angle", 45.0);
        row("clock_wipe", "clock_wipe");
        row("clock_wipe counter", "clock_wipe", "clockwise", false);
        row("radial_wipe", "radial_wipe");
        row("spiral_wipe", "spiral_wipe");
        row("venetian_blind_wipe", "venetian_blind_wipe");
        row("x_wipe", "x_wipe");
        row("barn_door", "barn_door");
        row("barn_door horizontal", "barn_door", "preset", QStringLiteral("horizontal"));
        row("barn_door reverse", "barn_door", "reverse", true);
        row("barn_door blur", "barn_door", "motionBlur", 50.0);
        row("slide", "slide");
        row("slide out", "slide", "mode", QStringLiteral("out"));
        row("slide up", "slide", "direction", QStringLiteral("up"));
        row("split", "split");
        row("split vertical", "split", "preset", QStringLiteral("vertical"));
        row("split blur", "split", "motionBlur", 50.0);
        row("brightness_flash", "brightness_flash");
        row("zoom_in", "zoom_in");
        row("zoom_in offset", "zoom_in", "offsetX", 30.0);
        row("zoom_in_out", "zoom_in_out");
        row("crash_zoom", "crash_zoom");
        row("rotate", "rotate");
        row("rotate_90", "rotate_90");
        row("noise_dissolve", "noise_dissolve");
        row("noise_dissolve soft", "noise_dissolve", "softness", 80.0);
        row("glitch", "glitch");
        row("camera_shake", "camera_shake");
        row("circles", "circles");
        row("circles few", "circles", "count", 2.0);
        row("flip_3d", "flip_3d");
        row("slice_push", "slice_push");
        row("warp", "warp");
        row("drop_warp", "drop_warp");
        row("tunnel_of_light", "tunnel_of_light");
        row("burn_away", "burn_away");
        row("burn_away angled", "burn_away", "angle", 135.0);
    }
    void renders()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        QFETCH(QString, id);
        QFETCH(QString, key);
        QFETCH(QVariant, value);
        QFETCH(bool, differsFromDissolve);
        const TimelineData tl = transitionTimeline(makeTransition(id, key, value));
        const QImage refA = render(*m_gpu, soloTimeline(generators::ColorBars), kMiddle);
        const QImage refB = render(*m_gpu, soloTimeline(generators::FourColorGradient), kMiddle);
        const QImage dissolve = render(*m_gpu,
            transitionTimeline(makeTransition(QStringLiteral("cross_dissolve"), {}, {})), kMiddle);

        // The first frame of the window (progress 1/26): still mostly the outgoing picture.
        const QImage start = render(*m_gpu, tl, kStart);
        QVERIFY2(meanDifference(start, refA) < meanDifference(start, refB) * 0.5,
            qPrintable(QString("to A %1, to B %2")
                    .arg(meanDifference(start, refA))
                    .arg(meanDifference(start, refB))));

        // Progress 0.5: neither picture, and (where it should) not the plain dissolve.
        const QImage mid = render(*m_gpu, tl, kMiddle);
        QVERIFY2(
            meanDifference(mid, refA) > 3, qPrintable(QString::number(meanDifference(mid, refA))));
        QVERIFY2(
            meanDifference(mid, refB) > 3, qPrintable(QString::number(meanDifference(mid, refB))));
        if (differsFromDissolve) {
            QVERIFY2(meanDifference(mid, dissolve) > 2,
                qPrintable(QString::number(meanDifference(mid, dissolve))));
        } else {
            QVERIFY2(meanDifference(mid, dissolve) < 2,
                qPrintable(QString::number(meanDifference(mid, dissolve))));
        }

        // Progress 25/26: close to the incoming picture.
        const QImage end = render(*m_gpu, tl, kEnd);
        QVERIFY2(meanDifference(end, refB) < meanDifference(end, refA) * 0.5,
            qPrintable(QString("to B %1, to A %2")
                    .arg(meanDifference(end, refB))
                    .arg(meanDifference(end, refA))));
    }

    /// Late in a long transition the shapes have almost covered the frame (what is left is
    /// the part of the frame the farthest corner still holds back).
    void shapesAlmostCoverTheFrameLate()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        const QImage refA = render(*m_gpu, soloTimeline(generators::ColorBars), 78);
        const QImage refB = render(*m_gpu, soloTimeline(generators::FourColorGradient), 78);
        for (const char* id : {"cross_iris", "diamond_iris", "eye_iris", "oval_iris", "heart_shape",
                 "star_shape", "arrow_iris", "box_shape", "spiral_wipe", "x_wipe", "clock_wipe",
                 "radial_wipe", "venetian_blind_wipe", "band_wipe", "center_wipe", "circles",
                 "noise_dissolve", "burn_away", "drop_warp", "slice_push"}) {
            TransitionData t = makeTransition(QString::fromLatin1(id), {}, {});
            t.duration = 60; // window [20, 81): frame 78 is at progress 0.95
            const QImage late = render(*m_gpu, transitionTimeline(t), 78);
            QVERIFY2(meanDifference(late, refB) < meanDifference(late, refA) * 0.25, id);
        }
    }

    void cpuFallbackIsADissolve()
    {
        const TimelineData cross
            = transitionTimeline(makeTransition(QStringLiteral("cross_dissolve"), {}, {}));
        const QImage expected = render(*m_cpu, cross, kMiddle);
        for (const char* id :
            {"smooth_cut", "star_shape", "barn_door", "venetian_blind_wipe", "blur_dissolve"}) {
            const TimelineData tl
                = transitionTimeline(makeTransition(QString::fromLatin1(id), {}, {}));
            QVERIFY2(meanDifference(render(*m_cpu, tl, kMiddle), expected) < 0.01, id);
        }
    }

    // ---- Saving ----

    void saveAndLoad_data() { registry_data(); }
    void saveAndLoad()
    {
        QFETCH(QString, id);
        TimelineData tl = transitionTimeline(makeTransition(id, {}, {}));
        ClipData& clip = tl.videoTracks[0].clips[1];
        clip.transitionIn->params.setBase(QStringLiteral("feather"), 30.0);
        clip.transitionIn->params.setStatic(QStringLiteral("color"), QColor(10, 20, 30));
        const ClipData back = serialization::clipFromJson(serialization::clipToJson(clip));
        QCOMPARE(back, clip);
        QCOMPARE(back.transitionIn->type, id);
    }

    // ---- Smooth Cut ----

    /// A white block jumps 24 output pixels to the right at the cut. The morph at 0.5 shows it
    /// between the two places; a dissolve would show two half-bright blocks.
    void smoothCutFindsTheMovedBlock()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        auto build = [&](const QString& type) {
            TimelineData tl = emptyTimeline();
            addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 100));
            auto block = [&](int64_t start, int64_t duration, double x) {
                ClipData c = solidClip(tl, Qt::white, start, duration);
                c.transform.setBase(keys::ZoomX, 0.2);
                c.transform.setBase(keys::ZoomY, 0.2);
                c.transform.setBase(keys::PosX, x);
                return c;
            };
            addClip(tl, TrackType::Video, 1, block(0, 50, -6));
            ClipData after = block(50, 50, 6);
            after.transitionIn = makeTransition(type, {}, {});
            addClip(tl, TrackType::Video, 1, after);
            return tl;
        };
        const QSize size(640, 360); // block: 128 x 72 px, centres at 308 and 332
        const QImage morph = render(*m_gpu, build(QStringLiteral("smooth_cut")), kMiddle, size);
        const QImage blend = render(*m_gpu, build(QStringLiteral("cross_dissolve")), kMiddle, size);
        auto at = [&](const QImage& img, int x) { return px(img, x, 180); };
        // The dissolve ghosts: half bright where only one of the blocks is.
        QVERIFY2(at(blend, 250).r > 100 && at(blend, 250).r < 160, describe(at(blend, 250)));
        QVERIFY2(at(blend, 390).r > 100 && at(blend, 390).r < 160, describe(at(blend, 390)));
        // The morph has one block in the middle (320 +- 64) and nothing at the two old edges.
        QVERIFY2(at(morph, 320).r > 230, describe(at(morph, 320)));
        QVERIFY2(at(morph, 250).r < 60, describe(at(morph, 250)));
        QVERIFY2(at(morph, 390).r < 60, describe(at(morph, 390)));
        // Start and end are the pictures themselves.
        QVERIFY(near(at(render(*m_gpu, build(QStringLiteral("smooth_cut")), kStart, size), 250),
            255, 255, 255, 255, 2));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestTransitions>(argc, argv);
}

#include "test_transitions.moc"
