// SPDX-License-Identifier: GPL-3.0-or-later
// The video transitions of the Effects library folders (spec 53): the registry rows (folder and
// parameters of every transition), one table-driven render test for all of them (the first frame
// is mostly the outgoing picture, the last mostly the incoming one, the middle differs from both
// and, where it should, from a plain dissolve), the CPU fallback (a plain dissolve), saving and
// loading, and Smooth Cut on synthetic frames. GPU cases skip without OpenGL.
#include "RenderTestUtil.h"
#include "TransitionRig.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Serialization.h"
#include "render/Compositor.h"
#include "render/GlRenderer.h"

#include <QOffscreenSurface>

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
    std::unique_ptr<TransitionRig> m_rig; ///< direct drawing (endpointsAreExact), made on first use
    bool m_rigOk = false;

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
        m_rig.reset(); // before the application goes
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
        const QString motion = QStringLiteral("color,border,feather");
        auto row = [](const char* id, const char* group, const QString& keys) {
            QTest::newRow(id) << QString::fromLatin1(id) << QString::fromLatin1(group) << keys;
        };
        row("cross_dissolve", "dissolve", ratios + ",reverse");
        row("dip_to_color", "dissolve", "color," + ratios + ",reverse");
        row("additive_dissolve", "dissolve", ratios + ",reverse");
        row("blur_dissolve", "dissolve", ratios + ",reverse,blurH,blurV");
        row("non_additive_dissolve", "dissolve", ratios + ",reverse");
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
        row("wipe", "wipe", "direction,softness,color,border,reverse");
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
        const QString curve = QStringLiteral("curve,power");
        for (const char* way : {"left", "right", "up", "down"}) {
            row(qPrintable(QStringLiteral("slide_%1").arg(QLatin1String(way))),
                "fusion_transitions", curve);
            row(qPrintable(QStringLiteral("pan_%1").arg(QLatin1String(way))), "fusion_transitions",
                "motionBlur," + curve);
        }
        row("fall_and_bounce", "fusion_transitions", "angle");
        row("film_strip", "fusion_transitions", "color,zoom," + curve);
        row("foreground_wipe", "fusion_transitions", "invert,softness,border,color");
        row("paint_on", "fusion_transitions", "count,shadow");
        row("round_and_down", "fusion_transitions", "angle,offsetX,offsetY," + curve);
        row("burn_away", "resolve_fx_transitions", "angle,raggedness,glow,char");
        row("push", "motion", "direction," + motion);
        row("barn_door", "motion", "preset,reverse,motionBlur," + motion);
        row("slide", "motion", "direction,mode,motionBlur," + motion);
        row("split", "motion", "preset,reverse,motionBlur," + motion);
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
        row("slide_left", "slide_left");
        row("slide_right easing", "slide_right", "curve", QStringLiteral("easing"));
        row("slide_up custom", "slide_up", "curve", QStringLiteral("custom"));
        row("slide_down", "slide_down");
        row("pan_left", "pan_left");
        row("pan_right", "pan_right");
        row("pan_up", "pan_up");
        row("pan_down", "pan_down");
        row("pan_left no blur", "pan_left", "motionBlur", 0.0);
        row("fall_and_bounce", "fall_and_bounce");
        row("fall_and_bounce sideways", "fall_and_bounce", "angle", 0.0);
        row("film_strip", "film_strip");
        row("film_strip colour", "film_strip", "color", QColor(200, 30, 30));
        row("foreground_wipe", "foreground_wipe");
        row("foreground_wipe inverted", "foreground_wipe", "invert", true);
        row("paint_on", "paint_on");
        row("paint_on few", "paint_on", "count", 3.0);
        row("round_and_down", "round_and_down");
        row("round_and_down pivot", "round_and_down", "offsetX", 0.0);
        row("wipe border", "wipe", "border", 30.0);
        row("wipe reverse", "wipe", "reverse", true);
        row("push border", "push", "border", 30.0);
        row("barn_door border", "barn_door", "border", 30.0);
        row("slide border", "slide", "border", 30.0);
        row("split border", "split", "border", 30.0);
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
        for (const char* id :
            {"cross_iris", "diamond_iris", "eye_iris", "oval_iris", "heart_shape", "star_shape",
                "arrow_iris", "box_shape", "spiral_wipe", "x_wipe", "clock_wipe", "radial_wipe",
                "venetian_blind_wipe", "band_wipe", "center_wipe", "circles", "noise_dissolve",
                "burn_away", "drop_warp", "slice_push", "slide_left", "pan_up", "fall_and_bounce",
                "film_strip", "foreground_wipe", "paint_on", "round_and_down"}) {
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

    // ---- Parameters added later (start / end ratio, reverse, border) ----

    /// A project saved before the parameters existed has none of them in its transition; it
    /// loads and renders exactly like a transition made now with the defaults.
    void oldProjectsRenderTheSame_data()
    {
        QTest::addColumn<QString>("id");
        for (const char* id :
            {"cross_dissolve", "dip_to_color", "wipe", "push", "additive_dissolve",
                "non_additive_dissolve", "blur_dissolve", "barn_door", "slide", "split"})
            QTest::newRow(id) << QString::fromLatin1(id);
    }
    void oldProjectsRenderTheSame()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        QFETCH(QString, id);
        // What the older versions stored: the transition without the parameters added since.
        static const QHash<QString, QStringList> kAdded {
            {"cross_dissolve", {"startRatio", "endRatio", "reverse"}},
            {"dip_to_color", {"startRatio", "endRatio", "reverse"}},
            {"additive_dissolve", {"reverse"}}, {"non_additive_dissolve", {"reverse"}},
            {"blur_dissolve", {"reverse"}}, {"wipe", {"color", "border", "reverse"}},
            {"push", {"color", "border", "feather"}}, {"barn_door", {"color", "border", "feather"}},
            {"slide", {"color", "border", "feather"}}, {"split", {"color", "border", "feather"}}};
        TransitionData fresh = makeTransition(id, {}, {});
        TransitionData old = fresh;
        for (const QString& key : kAdded.value(id)) {
            old.params.values.remove(key);
            old.params.statics.remove(key);
        }
        TimelineData tl = transitionTimeline(old);
        // The round trip through the project file keeps it that way.
        ClipData& clip = tl.videoTracks[0].clips[1];
        clip = serialization::clipFromJson(serialization::clipToJson(clip));
        QVERIFY(!clip.transitionIn->params.values.contains(QStringLiteral("border")));
        QCOMPARE(clip.transitionIn->params, old.params);
        for (const int64_t frame : {kStart, kMiddle, kEnd}) {
            QVERIFY2(meanDifference(render(*m_gpu, tl, frame),
                         render(*m_gpu, transitionTimeline(fresh), frame))
                    < 0.01,
                qPrintable(QString::number(frame)));
        }
    }

    /// Cross Dissolve at progress 0.5 is the plain average of the two pictures.
    void crossDissolveIsTheAverage()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        const QImage a = render(*m_gpu, soloTimeline(generators::ColorBars), kMiddle);
        const QImage b = render(*m_gpu, soloTimeline(generators::FourColorGradient), kMiddle);
        const QImage mid = render(*m_gpu,
            transitionTimeline(makeTransition(QStringLiteral("cross_dissolve"), {}, {})), kMiddle);
        QImage average = a;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
                average.setPixel(x, y,
                    qRgb((qRed(p) + qRed(q)) / 2, (qGreen(p) + qGreen(q)) / 2,
                        (qBlue(p) + qBlue(q)) / 2));
            }
        }
        QVERIFY2(meanDifference(mid, average) < 1.5,
            qPrintable(QString::number(meanDifference(mid, average))));
    }

    /// Start Ratio / End Ratio / Reverse of the dissolves.
    void dissolveRatios_data()
    {
        QTest::addColumn<QString>("id");
        for (const char* id : {"cross_dissolve", "dip_to_color", "additive_dissolve",
                 "non_additive_dissolve", "blur_dissolve"})
            QTest::newRow(id) << QString::fromLatin1(id);
    }
    void dissolveRatios()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        QFETCH(QString, id);
        const QImage refA = render(*m_gpu, soloTimeline(generators::ColorBars), kMiddle);
        const QImage refB = render(*m_gpu, soloTimeline(generators::FourColorGradient), kMiddle);
        auto draw = [&](double start, double end, bool reverse, int64_t frame) {
            TransitionData t = makeTransition(id, "startRatio", start);
            t.params.setBase(QStringLiteral("endRatio"), end);
            t.params.setStatic(QStringLiteral("reverse"), reverse);
            return render(*m_gpu, transitionTimeline(t), frame);
        };
        // Both ratios at 100 %: the incoming picture from the first frame (the dip's middle
        // is the colour, so look at its last quarter).
        const int64_t late = id == QLatin1String("dip_to_color") ? kEnd : kStart;
        QVERIFY(meanDifference(draw(100, 100, false, late), refB)
            < meanDifference(draw(100, 100, false, late), refA) * 0.3);
        // Both at 0 %: the outgoing picture to the end.
        const int64_t early = id == QLatin1String("dip_to_color") ? kStart : kEnd;
        QVERIFY(meanDifference(draw(0, 0, false, early), refA)
            < meanDifference(draw(0, 0, false, early), refB) * 0.3);
        // Reverse plays the curve from the other end: 0 -> 50 % becomes 50 -> 100 %.
        const QImage normal = draw(0, 50, false, kEnd);
        const QImage reversed = draw(0, 50, true, kEnd);
        const QImage explicitReverse = draw(50, 100, false, kEnd);
        QVERIFY(meanDifference(normal, reversed) > 3);
        QVERIFY(meanDifference(reversed, explicitReverse) < 0.01);
    }

    /// With a border the edge of the wipes and motions carries a band of the border colour.
    void borderColourShowsAtTheEdge_data()
    {
        QTest::addColumn<QString>("id");
        for (const char* id : {"wipe", "push", "barn_door", "slide", "split", "foreground_wipe"})
            QTest::newRow(id) << QString::fromLatin1(id);
    }
    void borderColourShowsAtTheEdge()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        QFETCH(QString, id);
        const auto magenta = [](const QImage& img) {
            int n = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    const QRgb p = img.pixel(x, y);
                    n += qRed(p) > 230 && qGreen(p) < 30 && qBlue(p) > 230;
                }
            }
            return n;
        };
        TransitionData t = makeTransition(id, "color", QColor(255, 0, 255));
        t.params.setBase(QStringLiteral("border"), 12.0);
        TransitionData none = t;
        none.params.setBase(QStringLiteral("border"), 0.0);
        const QImage with = render(*m_gpu, transitionTimeline(t), kMiddle);
        QVERIFY2(magenta(with) > 100, qPrintable(QString::number(magenta(with))));
        QCOMPARE(magenta(render(*m_gpu, transitionTimeline(none), kMiddle)), 0);
    }

    // ---- Exact endpoints ----

    /// Every transition drawn directly at progress 0 is the outgoing picture and at progress 1
    /// the incoming one. Not meaningful (listed with the reason):
    ///  - smooth_cut: without the optical flow interpolator it is the plain dissolve; with it
    ///    the endpoints are the pictures themselves (covered in smoothCutFindsTheMovedBlock).
    /// Everything else, flashes and glitches included, has to match within the tolerance.
    void endpointsAreExact_data()
    {
        QTest::addColumn<QString>("id");
        for (const EffectDescriptor* d :
            EffectRegistry::instance().byCategory(EffectCategory::Transition))
            QTest::newRow(qPrintable(d->id)) << d->id;
    }
    void endpointsAreExact()
    {
        QFETCH(QString, id);
        QString why;
        if (!m_rig) {
            m_rig = std::make_unique<TransitionRig>();
            m_rigOk = m_rig->initialize(&why);
        }
        TransitionRig& rig = *m_rig;
        if (!m_rigOk)
            QSKIP(qPrintable("no OpenGL: " + why));
        const QSize size(192, 108);
        const QImage a = testPicture(false, size);
        const QImage b = testPicture(true, size);
        const ParamSet params = EffectRegistry::instance().defaultParams(id);
        QString error;
        const QImage atStart = rig.draw(id, params, 0.0, a, b, 0, &error);
        QVERIFY2(!atStart.isNull(), qPrintable("no picture: " + error));
        const QImage atEnd = rig.draw(id, params, 1.0, a, b, 25);
        // Mean difference per channel, and the worst single channel.
        const auto distance = [](const QImage& x, const QImage& y, int* worst) {
            double sum = 0;
            *worst = 0;
            for (int row = 0; row < x.height(); ++row) {
                for (int col = 0; col < x.width(); ++col) {
                    const QRgb p = x.pixel(col, row), q = y.pixel(col, row);
                    const int d[3] = {std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                        std::abs(qBlue(p) - qBlue(q))};
                    sum += d[0] + d[1] + d[2];
                    *worst = std::max({*worst, d[0], d[1], d[2]});
                }
            }
            return sum / (3.0 * x.width() * x.height());
        };
        int worstStart = 0, worstEnd = 0;
        const double meanStart = distance(atStart, a, &worstStart);
        const double meanEnd = distance(atEnd, b, &worstEnd);
        QVERIFY2(meanStart < 1.0 && worstStart < 40,
            qPrintable(QString("progress 0: mean %1, worst %2").arg(meanStart).arg(worstStart)));
        QVERIFY2(meanEnd < 1.0 && worstEnd < 40,
            qPrintable(QString("progress 1: mean %1, worst %2").arg(meanEnd).arg(worstEnd)));
    }

    // ---- Shaders build when first used ----

    /// A fresh GL renderer has built only the fallback transition (cross_dissolve) and no
    /// effect; drawing another transition builds that one, once, and renders like the
    /// compositor does; the same for an effect on a clip.
    void shadersBuildWhenFirstUsed()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        QOffscreenSurface surface;
        surface.create();
        render::GlRenderer renderer(&surface);
        QString why;
        if (!renderer.initialize(&why))
            QSKIP(qPrintable("renderer: " + why));
        QCOMPARE(renderer.builtTransitionCount(), 1);
        QCOMPARE(renderer.builtEffectCount(), 0);

        auto draw = [&](const TimelineData& tl, int64_t frame) {
            render::RenderJob job;
            job.request.timeline = snap(tl);
            job.request.frame = frame;
            job.request.outputs = OutputImage;
            job.media = std::make_shared<MediaPathMap>();
            auto future = job.promise.get_future();
            renderer.render(job);
            renderer.finishPending(false);
            const RenderedFramePtr frameResult = future.get();
            return frameResult ? frameResult->image : QImage();
        };

        // A frame without a transition builds nothing.
        draw(soloTimeline(generators::ColorBars), 10);
        QCOMPARE(renderer.builtTransitionCount(), 1);

        const TimelineData star
            = transitionTimeline(makeTransition(QStringLiteral("star_shape"), {}, {}));
        const QImage lazy = draw(star, kMiddle);
        QCOMPARE(renderer.builtTransitionCount(), 2);
        QVERIFY(meanDifference(lazy, render(*m_gpu, star, kMiddle)) < 0.5);
        const QImage refA = render(*m_gpu, soloTimeline(generators::ColorBars), kMiddle);
        QVERIFY(meanDifference(lazy, refA) > 3); // it is not just the outgoing picture

        draw(star, kStart); // the same transition again: nothing more is built
        QCOMPARE(renderer.builtTransitionCount(), 2);
        // A transition id with no GPU implementation is drawn as the fallback, builds nothing.
        TransitionData unknown = makeTransition(QStringLiteral("cross_dissolve"), {}, {});
        unknown.type = QStringLiteral("no_such_transition");
        const QImage fallback = draw(transitionTimeline(unknown), kMiddle);
        QCOMPARE(renderer.builtTransitionCount(), 2);
        QVERIFY(
            meanDifference(fallback,
                render(*m_gpu,
                    transitionTimeline(makeTransition(QStringLiteral("cross_dissolve"), {}, {})),
                    kMiddle))
            < 0.5);

        // An effect builds on the first frame that uses it.
        TimelineData blurred = soloTimeline(generators::ColorBars);
        ClipData& clip = blurred.videoTracks[0].clips[0];
        clip.effects.append(EffectRegistry::instance().createEffect(
            QStringLiteral("gaussian_blur"), blurred.allocateId()));
        clip.effects[0].params.setBase(QStringLiteral("radius"), 6.0);
        const QImage soft = draw(blurred, 10);
        QCOMPARE(renderer.builtEffectCount(), 1);
        QVERIFY(meanDifference(soft, render(*m_gpu, blurred, 10)) < 0.5);
        QVERIFY(meanDifference(soft, render(*m_gpu, soloTimeline(generators::ColorBars), 10)) > 1);
        renderer.shutdown();
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
