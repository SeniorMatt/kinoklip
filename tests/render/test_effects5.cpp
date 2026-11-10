// SPDX-License-Identifier: GPL-3.0-or-later
// The multi-frame effects of the 1.2 gap list (docs/behavior-spec.md section 60): Motion Trails,
// Smear, Frame Replacer, Motion Blur, Deflicker, Color Stabilizer, Automatic Dirt Removal, Dust
// Buster and Patch Replacer. The temporal tests run on synthetic footage made with the ffmpeg
// command line at the start (a white square that moves 8 px per frame, a dust dot that shows in
// one frame, a flickering and a colour-drifting grey scene; lossless, so the expected values
// are exact) and on the barcode fixture, whose frame index tells which frame was read. GPU cases
// skip without OpenGL. Also writes the contact sheet effects-5.png when KK_EFFECT_SHEETS is set.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "render/Compositor.h"
#include "render/EffectMathTemporal.h"
#include "render/GlEffects.h"
#include "render/GlTemporalBase.h"
#include "render/TimeEffects.h"
#include "render/TimelineEval.h"

#include <QDir>
#include <QPainter>
#include <QTemporaryDir>

#include <set>

using namespace kk;
using namespace kk::test;
using namespace kk::render;

namespace {

namespace g = generators;
namespace k = fxkeys;

constexpr int kW = 320, kH = 180;

/// All new video effects: id, folder, display name.
struct Row
{
    const char* id;
    const char* group;
    const char* name;
};
const Row kEffects[] = {
    {fxid::MotionTrails, effectgroups::Temporal, "Motion Trails"},
    {fxid::Smear, effectgroups::Temporal, "Smear"},
    {fxid::TemporalMotionBlur, effectgroups::Temporal, "Motion Blur"},
    {fxid::FrameReplacer, effectgroups::Revival, "Frame Replacer"},
    {fxid::Deflicker, effectgroups::Revival, "Deflicker"},
    {fxid::DirtRemoval, effectgroups::Revival, "Automatic Dirt Removal"},
    {fxid::DustBuster, effectgroups::Revival, "Dust Buster"},
    {fxid::PatchReplacer, effectgroups::Revival, "Patch Replacer"},
    {fxid::ColorStabilizer, effectgroups::Color, "Color Stabilizer"},
};

using Values = std::initializer_list<std::pair<const QString, double>>;
using Statics = std::initializer_list<std::pair<const QString, QVariant>>;

EffectInstance effect(TimelineData& tl, const char* type, Values values = {}, Statics statics = {})
{
    EffectInstance e
        = EffectRegistry::instance().createEffect(QLatin1String(type), tl.allocateId());
    for (const auto& [key, v] : values)
        e.params.setBase(key, v);
    for (const auto& [key, v] : statics)
        e.params.setStatic(key, v);
    return e;
}

int maxDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q))});
        }
    }
    return worst;
}

int grey(const QImage& img, int x, int y)
{
    return qGray(img.pixel(x, y));
}

double meanChannel(const QImage& img, int channel, QRect rect)
{
    double sum = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const QRgb p = img.pixel(x, y);
            sum += channel == 0 ? qRed(p) : channel == 1 ? qGreen(p) : qBlue(p);
        }
    }
    return sum / (rect.width() * rect.height());
}

double stddev(const QVector<double>& v)
{
    double mean = 0, var = 0;
    for (const double x : v)
        mean += x;
    mean /= v.size();
    for (const double x : v)
        var += (x - mean) * (x - mean);
    return std::sqrt(var / v.size());
}

} // namespace

class TestEffects5 : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    QTemporaryDir m_dir;
    QString m_footageError;
    QString m_fixtureError;

    bool gpuReady() const { return m_gpu && m_gpu->isGpu(); }

    /// Makes @p name.mkv (320 x 180, 25 fps, lossless, 40 frames) from the ffmpeg filter graph.
    bool makeFootage(const QString& name, const QStringList& inputs, const QString& graph)
    {
        QStringList args {QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error")};
        for (const QString& in : inputs)
            args << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i") << in;
        args << QStringLiteral("-filter_complex") << graph << QStringLiteral("-map")
             << QStringLiteral("[out]") << QStringLiteral("-frames:v") << QStringLiteral("40")
             << QStringLiteral("-c:v") << QStringLiteral("ffv1") << QStringLiteral("-pix_fmt")
             << QStringLiteral("yuv420p") << m_dir.filePath(name + QStringLiteral(".mkv"));
        QProcess ffmpeg;
        ffmpeg.setProcessChannelMode(QProcess::MergedChannels);
        ffmpeg.start(QStringLiteral("ffmpeg"), args);
        if (!ffmpeg.waitForFinished(120000) || ffmpeg.exitCode() != 0) {
            m_footageError = name + QStringLiteral(": ") + QString::fromLocal8Bit(ffmpeg.readAll());
            return false;
        }
        return true;
    }

    QImage render(const TimelineData& tl, int64_t frame)
    {
        return m_gpu->renderImage(snap(tl), frame);
    }

    /// Frame @p frame of the footage @p media (a clip of 40 frames) with @p effects.
    QImage footage(const QString& media, const QVector<EffectInstance>& effects, int64_t frame,
        double speed = 1.0, int64_t sourceInUs = 0)
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = mediaClip(tl, media, 0, 40, sourceInUs, 0);
        clip.speed = speed;
        clip.effects = effects;
        addClip(tl, TrackType::Video, 0, std::move(clip));
        return render(tl, frame);
    }

    /// Mean grey of every frame in [first, last] of @p media with @p effects.
    QVector<double> frameMeans(
        const QString& media, const QVector<EffectInstance>& effects, int first, int last)
    {
        QVector<double> means;
        for (int f = first; f <= last; ++f) {
            const QImage img = footage(media, effects, f);
            double sum = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    sum += qGray(img.pixel(x, y));
            means.append(sum / (img.width() * img.height()));
        }
        return means;
    }

    TimelineData emptyT() { return emptyTimeline(kW, kH); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_gpu = std::make_unique<Compositor>();
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
        const QString bg = QStringLiteral("color=c=0x303030:s=320x180:r=25");
        const QString square = QStringLiteral("color=c=white:s=40x40:r=25");
        const QString dot = QStringLiteral("color=c=white:s=6x6:r=25");
        // A white 40 x 40 square, 8 px further right every frame, on dark grey.
        bool ok = makeFootage(QStringLiteral("motion"), {bg, square},
            QStringLiteral("[0][1]overlay=x='(n-1)*8':y=70[out]"));
        // A square moving 6 px per frame, a dust dot in frame 10 at (200, 30), another at
        // (260, 150), and a dot that stays two frames (20 and 21) at (40, 140).
        ok = ok
            && makeFootage(QStringLiteral("dirt"), {bg, square, dot},
                QStringLiteral("[0][1]overlay=x='(n-1)*6':y=70[a];"
                               "[a][2]overlay=x=200:y=30:enable='eq(n,10)'[b];"
                               "[b][2]overlay=x=260:y=150:enable='eq(n,10)'[c];"
                               "[c][2]overlay=x=40:y=140:enable='between(n,20,21)'[out]"));
        // A grey scene with a grid whose brightness flickers.
        ok = ok
            && makeFootage(QStringLiteral("flicker"),
                {QStringLiteral("color=c=0x707070:s=320x180:r=25")},
                QStringLiteral("[0]drawgrid=w=40:h=40:t=2:c=white@0.5,"
                               "eq=brightness='0.12*sin(n*2.1)+0.08*sin(n*0.7)':eval=frame[out]"));
        // The same scene whose red drifts up and down.
        ok = ok
            && makeFootage(QStringLiteral("tint"),
                {QStringLiteral("color=c=0x707070:s=320x180:r=25")},
                QStringLiteral("[0]drawgrid=w=40:h=40:t=2:c=white@0.5,format=gbrp,"
                               "geq=r='r(X,Y)*(1+0.25*sin(N*1.3))':g='g(X,Y)':b='b(X,Y)',"
                               "format=yuv420p[out]"));
        auto map = std::make_shared<MediaPathMap>();
        for (const char* name : {"motion", "dirt", "flicker", "tint"})
            map->insert(
                QLatin1String(name), m_dir.filePath(QLatin1String(name) + QStringLiteral(".mkv")));
        QString why;
        if (ensureFixtures(&why))
            map->insert(QStringLiteral("v"), fixture("h264_25fps.mp4"));
        else
            m_fixtureError = why;
        m_gpu->setMediaPaths(map);
        if (!ok)
            qWarning("footage: %s", qPrintable(m_footageError));
    }
    void cleanupTestCase() { m_gpu.reset(); }

    void footageWasMade() { QVERIFY2(m_footageError.isEmpty(), qPrintable(m_footageError)); }

    // ---- registry ----

    void effectsAreRegistered()
    {
        const EffectRegistry& registry = EffectRegistry::instance();
        std::set<QString> gpuIds;
        for (const GlEffectFactory& f : builtinGlEffects())
            gpuIds.insert(QLatin1String(f.id));
        for (const Row& r : kEffects) {
            const EffectDescriptor* d = registry.find(QLatin1String(r.id));
            QVERIFY2(d, r.id);
            QCOMPARE(d->category, EffectCategory::VideoEffect);
            QCOMPARE(d->group, QLatin1String(r.group));
            QCOMPARE(d->name, QLatin1String(r.name));
            QVERIFY(!d->description.isEmpty());
            QVERIFY2(gpuIds.count(d->id), r.id);
            QVERIFY2(!d->params.isEmpty(), r.id);
            const ParamSet defaults = registry.defaultParams(d->id);
            for (const ParamDescriptor& p : d->params) {
                if (p.isScalar()) {
                    QVERIFY2(p.def >= p.min && p.def <= p.max, qPrintable(d->id + p.key));
                    QCOMPARE(defaults.value(p.key, 0, -12345.0), p.def);
                } else {
                    QVERIFY2(defaults.statics.contains(p.key), qPrintable(d->id + p.key));
                    if (p.type == ParamType::Choice)
                        QVERIFY(p.choices.contains(p.defStatic.toString()));
                }
            }
        }
        QVERIFY(registry.groups(EffectCategory::VideoEffect)
                    .contains(QLatin1String(effectgroups::Temporal)));
        QCOMPARE(EffectRegistry::groupName(effectgroups::Temporal), QStringLiteral("Temporal"));
        // Frame counts cannot be keyframed (they decide which frames are decoded); the strengths
        // can.
        for (const char* id :
            {fxid::MotionTrails, fxid::Smear, fxid::Deflicker, fxid::DirtRemoval}) {
            const EffectDescriptor* d = registry.find(QLatin1String(id));
            for (const ParamDescriptor& p : d->params) {
                if (p.key == k::Trails || p.key == k::FrameWindow || p.key == k::Length)
                    QVERIFY2(!p.animatable, id);
                if (p.key == k::Amount || p.key == k::Strength)
                    QVERIFY2(p.animatable, id);
            }
        }
    }

    // ---- which frames are read (no GPU) ----

    void framesReadFollowTheEffect()
    {
        const EffectRegistry& registry = EffectRegistry::instance();
        auto read = [&](const char* id, int64_t f, Values values = {}, Statics statics = {}) {
            TimelineData tl = emptyT();
            const EffectInstance e = effect(tl, id, values, statics);
            std::vector<int64_t> out;
            timefx::framesRead(QLatin1String(id), e.params, f, &out);
            return out;
        };
        using V = std::vector<int64_t>;
        QCOMPARE(read(fxid::MotionTrails, 10, {{k::Trails, 3.0}, {k::Delay, 2.0}}), (V {8, 6, 4}));
        QCOMPARE(read(fxid::MotionTrails, 10, {{k::Opacity, 0.0}}), V {});
        QCOMPARE(read(fxid::Smear, 10, {{k::Length, 2.0}}), (V {9, 11, 8, 12}));
        QCOMPARE(read(fxid::Smear, 10, {{k::Length, 2.0}}, {{k::SmearDirection, fxchoice::Behind}}),
            (V {9, 8}));
        QCOMPARE(read(fxid::Smear, 10, {{k::Length, 2.0}}, {{k::SmearDirection, fxchoice::Ahead}}),
            (V {11, 12}));
        QCOMPARE(read(fxid::FrameReplacer, 10), (V {9}));
        QCOMPARE(read(fxid::FrameReplacer, 10, {}, {{k::FrameSource, fxchoice::Next}}), (V {11}));
        QCOMPARE(read(fxid::FrameReplacer, 10, {}, {{k::FrameSource, fxchoice::Mix}}), (V {9, 11}));
        QCOMPARE(read(fxid::FrameReplacer, 10, {{k::Amount, 0.0}}), V {});
        QCOMPARE(read(fxid::TemporalMotionBlur, 10), (V {9, 11}));
        QCOMPARE(read(fxid::TemporalMotionBlur, 10, {{k::ShutterAngle, 0.0}}), V {});
        QCOMPARE(read(fxid::Deflicker, 10, {{k::FrameWindow, 2.0}}), (V {9, 11, 8, 12}));
        QCOMPARE(read(fxid::DirtRemoval, 10), (V {9, 11}));
        QCOMPARE(read(fxid::DirtRemoval, 10, {{k::FrameWindow, 2.0}}), (V {9, 11, 8, 12}));
        QCOMPARE(read(fxid::DustBuster, 10, {{k::FrameWindow, 2.0}}), (V {9, 11, 8, 12}));
        QCOMPARE(read(fxid::ColorStabilizer, 10, {{k::ReferenceFrame, 3.0}}), (V {3}));
        QCOMPARE(read(fxid::ColorStabilizer, 3, {{k::ReferenceFrame, 3.0}}), V {}); // itself
        QCOMPARE(read(fxid::PatchReplacer, 10), V {}); // reads only its own frame
        // Every effect that reads frames is known to the renderer's decode planning.
        for (const char* id :
            {fxid::MotionTrails, fxid::Smear, fxid::FrameReplacer, fxid::TemporalMotionBlur,
                fxid::Deflicker, fxid::DirtRemoval, fxid::DustBuster, fxid::ColorStabilizer})
            QVERIFY2(timefx::isTimeEffect(QLatin1String(id)), id);
        QVERIFY(!timefx::isTimeEffect(QLatin1String(fxid::PatchReplacer)));
        // The window is bounded however much is stacked: at most kMaxClipFrames frames.
        TimelineData tl = emptyT();
        ClipData clip = mediaClip(tl, QStringLiteral("motion"), 0, 40);
        clip.effects = {effect(tl, fxid::Smear, {{k::Length, 2.0}}),
            effect(tl, fxid::Deflicker, {{k::FrameWindow, 2.0}}),
            effect(tl, fxid::MotionTrails, {{k::Trails, 5.0}}),
            effect(tl, fxid::DirtRemoval, {{k::FrameWindow, 2.0}})};
        const std::vector<int64_t> all = timefx::clipFrames(clip, 20);
        QCOMPARE(all.front(), int64_t(20));
        QVERIFY(int(all.size()) <= timefx::kMaxClipFrames);
        QCOMPARE(std::set<int64_t>(all.begin(), all.end()).size(), all.size()); // no duplicates
    }

    void neighboursFollowTrimSpeedAndReverse()
    {
        // The neighbours are the clip's own frame steps: the source time of local frame f - 1
        // and f + 1 comes from the clip's retime, so speed, reverse and trim are respected.
        TimelineData tl = emptyT();
        const Rational fps = tl.fps;
        const int64_t frameUs = 40000;
        ClipData clip = mediaClip(tl, QStringLiteral("v"), 0, 100, 2'000'000);
        const int64_t base = clip.sourceTimeUsAt(10, fps);
        QCOMPARE(base, 2'000'000 + 10 * frameUs);
        QCOMPARE(clip.sourceTimeUsAt(9, fps), base - frameUs);
        QCOMPARE(clip.sourceTimeUsAt(11, fps), base + frameUs);
        clip.speed = 2.0;
        QCOMPARE(clip.sourceTimeUsAt(11, fps) - clip.sourceTimeUsAt(10, fps), 2 * frameUs);
        QCOMPARE(clip.sourceTimeUsAt(10, fps) - clip.sourceTimeUsAt(9, fps), 2 * frameUs);
        clip.speed = -1.0;
        QCOMPARE(clip.sourceTimeUsAt(9, fps) - clip.sourceTimeUsAt(10, fps), frameUs);
        clip.speed = 0.0; // freeze: every neighbour is the same source frame
        QCOMPARE(clip.sourceTimeUsAt(9, fps), clip.sourceTimeUsAt(11, fps));
        // Before the clip start the trim point's earlier frames (the clip's handle) are read, and
        // the media's first frame repeats once the source time would go below 0.
        clip.speed = 1.0;
        QCOMPARE(clip.sourceTimeUsAt(-1, fps), 2'000'000 - frameUs);
        QCOMPARE(clampedSourceTimeUs(mediaClip(tl, QStringLiteral("v"), 0, 100, 0), -1, fps),
            int64_t(0));
    }

    // ---- the frames that come back (GPU, barcode fixture) ----

    void frameReplacerReadsTheRightFrames_data()
    {
        QTest::addColumn<double>("speed");
        QTest::addColumn<qlonglong>("sourceInUs");
        QTest::addColumn<int>("frame");
        QTest::addColumn<QString>("source");
        QTest::addColumn<int>("offset"); // the neighbour's frame step: -1 previous, +1 next
        QTest::newRow("previous") << 1.0 << 0LL << 10 << "previous" << -1;
        QTest::newRow("next") << 1.0 << 0LL << 10 << "next" << 1;
        QTest::newRow("trimmed previous") << 1.0 << 2'000'000LL << 5 << "previous" << -1;
        QTest::newRow("trimmed first frame, handle") << 1.0 << 2'000'000LL << 0 << "previous" << -1;
        QTest::newRow("speed 2 previous") << 2.0 << 0LL << 10 << "previous" << -1;
        QTest::newRow("speed 2 next") << 2.0 << 0LL << 10 << "next" << 1;
        QTest::newRow("reverse previous") << -1.0 << 6'000'000LL << 10 << "previous" << -1;
        QTest::newRow("reverse next") << -1.0 << 6'000'000LL << 10 << "next" << 1;
        QTest::newRow("media start repeats") << 1.0 << 0LL << 0 << "previous" << -1;
    }
    void frameReplacerReadsTheRightFrames()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QFAIL(qPrintable(m_fixtureError));
        QFETCH(double, speed);
        QFETCH(qlonglong, sourceInUs);
        QFETCH(int, frame);
        QFETCH(QString, source);
        QFETCH(int, offset);
        TimelineData tl = emptyTimeline(640, 360);
        const Rational fps = tl.fps;
        ClipData clip = mediaClip(tl, QStringLiteral("v"), 0, 100, sourceInUs);
        clip.speed = speed;
        const int64_t want = std::max<int64_t>(0, clip.sourceTimeUsAt(frame + offset, fps));
        clip.effects = {effect(tl, fxid::FrameReplacer, {}, {{k::FrameSource, source}})};
        addClip(tl, TrackType::Video, 0, clip);
        const QImage img = render(tl, frame);
        QCOMPARE(readFrameIndex(img), int(expectedFrameIndex(want, 25, 1)));
        // Without the effect the clip shows its own frame.
        tl.videoTracks[0].clips[0].effects.clear();
        QCOMPARE(readFrameIndex(render(tl, frame)),
            int(expectedFrameIndex(std::max<int64_t>(0, clip.sourceTimeUsAt(frame, fps)), 25, 1)));
    }

    // ---- neutral parameters ----

    void neutralParametersLeavePictureAlone()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        const QImage plain = footage(QStringLiteral("motion"), {}, 12);
        const struct
        {
            const char* type;
            Values values;
        } rows[] = {
            {fxid::MotionTrails, {{k::Opacity, 0.0}}},
            {fxid::Smear, {{k::Amount, 0.0}}},
            {fxid::FrameReplacer, {{k::Amount, 0.0}}},
            {fxid::TemporalMotionBlur, {{k::ShutterAngle, 0.0}}},
            {fxid::Deflicker, {{k::Strength, 0.0}}},
            {fxid::ColorStabilizer, {{k::Strength, 0.0}}},
            {fxid::DirtRemoval, {{k::Amount, 0.0}}},
            {fxid::DustBuster, {{k::Amount, 0.0}}},
            {fxid::PatchReplacer, {{k::Opacity, 0.0}}},
        };
        for (const auto& r : rows)
            QVERIFY2(
                maxDifference(
                    footage(QStringLiteral("motion"), {effect(t, r.type, r.values)}, 12), plain)
                    <= 1,
                r.type);
        // A picture that does not change stays as it is under the effects that average or trail
        // it, at the first frame too (frames before the clip repeat its edge frame).
        TimelineData tl = emptyT();
        ClipData bars;
        bars.id = tl.allocateId();
        bars.kind = ClipKind::Generator;
        bars.generator = g::ColorBars;
        bars.title = EffectRegistry::instance().defaultParams(g::ColorBars);
        bars.duration = 30;
        addClip(tl, TrackType::Video, 0, bars);
        const QImage still0 = render(tl, 0), still29 = render(tl, 29);
        for (const char* type : {fxid::Smear, fxid::MotionTrails, fxid::TemporalMotionBlur,
                 fxid::Deflicker, fxid::DirtRemoval, fxid::FrameReplacer}) {
            tl.videoTracks[0].clips[0].effects
                = {effect(tl, type, {}, {{k::TrailMode, fxchoice::Average}})};
            QVERIFY2(maxDifference(render(tl, 0), still0) <= 2, type);
            QVERIFY2(maxDifference(render(tl, 29), still29) <= 2, type);
        }
    }

    // ---- Motion Trails ----

    void motionTrailsLeaveATrailBehindTheSquare()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        // Frame 12: the square covers x = 96..135; earlier it was at 88 (11), 80 (10), 72 (9).
        const QImage plain = footage(QStringLiteral("motion"), {}, 12);
        QCOMPARE(grey(plain, 76, 90), grey(plain, 40, 90)); // plain: nothing behind the square
        QVERIFY(grey(plain, 110, 90) > 240);
        for (const QString& mode : {fxchoice::Lighten, fxchoice::Average}) {
            const QImage trails = footage(QStringLiteral("motion"),
                {effect(t, fxid::MotionTrails, {{k::Trails, 4.0}}, {{k::TrailMode, mode}})}, 12);
            const int behind = grey(trails, 76, 90), ahead = grey(trails, 150, 90);
            qInfo("trails %s: behind %d (plain %d), ahead %d (plain %d)", qPrintable(mode), behind,
                grey(plain, 76, 90), ahead, grey(plain, 150, 90));
            QVERIFY2(behind > grey(plain, 76, 90) + 10, qPrintable(mode)); // a trail behind
            QVERIFY2(std::abs(ahead - grey(plain, 150, 90)) <= 2, qPrintable(mode)); // none ahead
            QVERIFY2(std::abs(grey(trails, 20, 20) - grey(plain, 20, 20)) <= 2, qPrintable(mode));
        }
        // The trail fades: the part the square left a frame ago is brighter than the older part.
        const QImage fade = footage(QStringLiteral("motion"),
            {effect(t, fxid::MotionTrails, {{k::Trails, 4.0}, {k::Decay, 50.0}})}, 12);
        QVERIFY(grey(fade, 90, 90) > grey(fade, 74, 90)); // nearer to the square, newer, brighter
        QVERIFY(grey(fade, 74, 90) > grey(plain, 74, 90));
        // The first frame has no earlier frame: the picture is the plain one.
        QVERIFY(maxDifference(footage(QStringLiteral("motion"), {effect(t, fxid::MotionTrails)}, 0),
                    footage(QStringLiteral("motion"), {}, 0))
            <= 2);
    }

    // ---- Smear ----

    void smearAveragesAroundTheFrame()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        const QImage plain = footage(QStringLiteral("motion"), {}, 12);
        const int bg = grey(plain, 150, 90);
        auto smear = [&](const QString& direction) {
            return footage(QStringLiteral("motion"),
                {effect(t, fxid::Smear, {{k::Length, 2.0}, {k::Decay, 0.0}},
                    {{k::SmearDirection, direction}})},
                12);
        };
        const QImage both = smear(fxchoice::Both), behind = smear(fxchoice::Behind),
                     ahead = smear(fxchoice::Ahead);
        // Frame 12: x = 96..135. Frames 13 and 14 cover up to 143 and 151; 11 and 10 start at
        // 88, 80.
        for (const int x : {84, 90, 140, 148})
            qInfo("smear x=%d: plain %d both %d behind %d ahead %d", x, grey(plain, x, 90),
                grey(both, x, 90), grey(behind, x, 90), grey(ahead, x, 90));
        QVERIFY(grey(both, 140, 90) > bg + 20); // ahead of the square
        QVERIFY(grey(both, 84, 90) > bg + 20); // behind it
        QVERIFY(std::abs(grey(behind, 140, 90) - bg) <= 2);
        QVERIFY(grey(behind, 84, 90) > bg + 20);
        QVERIFY(grey(ahead, 140, 90) > bg + 20);
        QVERIFY(std::abs(grey(ahead, 84, 90) - bg) <= 2);
        // Equal weights, five frames: the pixel is the mean of what the five frames show.
        const QImage f10 = footage(QStringLiteral("motion"), {}, 10),
                     f11 = footage(QStringLiteral("motion"), {}, 11),
                     f13 = footage(QStringLiteral("motion"), {}, 13),
                     f14 = footage(QStringLiteral("motion"), {}, 14);
        for (const int x : {84, 100, 140, 148}) {
            const double want = (grey(plain, x, 90) + grey(f10, x, 90) + grey(f11, x, 90)
                                    + grey(f13, x, 90) + grey(f14, x, 90))
                / 5.0;
            QVERIFY2(std::abs(grey(both, x, 90) - want) <= 3, qPrintable(QString::number(x)));
        }
        // Amount 50 % is halfway between the picture and the smear.
        const QImage half = footage(QStringLiteral("motion"),
            {effect(t, fxid::Smear, {{k::Length, 2.0}, {k::Decay, 0.0}, {k::Amount, 50.0}})}, 12);
        QVERIFY(
            std::abs(grey(half, 140, 90) - (grey(plain, 140, 90) + grey(both, 140, 90)) / 2) <= 3);
    }

    // ---- Frame Replacer ----

    void frameReplacerMixesTheNeighbours()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        auto replaced = [&](const QString& source, double amount = 100.0, int frame = 12) {
            return footage(QStringLiteral("motion"),
                {effect(t, fxid::FrameReplacer, {{k::Amount, amount}}, {{k::FrameSource, source}})},
                frame);
        };
        const QImage previous = replaced(fxchoice::Previous), next = replaced(fxchoice::Next),
                     mix = replaced(fxchoice::Mix);
        QVERIFY(maxDifference(previous, footage(QStringLiteral("motion"), {}, 11)) <= 1);
        QVERIFY(maxDifference(next, footage(QStringLiteral("motion"), {}, 13)) <= 1);
        QVERIFY(maxDifference(previous, footage(QStringLiteral("motion"), {}, 12))
            > 100); // it does something
        for (const int x : {90, 100, 130, 140}) { // mix = mean of the two neighbours
            QVERIFY(
                std::abs(grey(mix, x, 90) - (grey(previous, x, 90) + grey(next, x, 90)) / 2) <= 2);
        }
        // 50 % is halfway between the frame and its neighbour; the amount can be keyframed.
        const QImage half = replaced(fxchoice::Previous, 50.0);
        QVERIFY(std::abs(grey(half, 130, 90) - (grey(previous, 130, 90) + 255) / 2) <= 3);
        TimelineData tl = emptyT();
        ClipData clip = mediaClip(tl, QStringLiteral("motion"), 0, 40);
        EffectInstance e = effect(tl, fxid::FrameReplacer);
        e.params.values[k::Amount].setKey(11, 0.0);
        e.params.values[k::Amount].setKey(12, 100.0);
        e.params.values[k::Amount].setKey(13, 0.0);
        clip.effects = {e};
        addClip(tl, TrackType::Video, 0, clip);
        QVERIFY(maxDifference(render(tl, 11), footage(QStringLiteral("motion"), {}, 11)) <= 1);
        QVERIFY(maxDifference(render(tl, 12), previous) <= 1);
        QVERIFY(maxDifference(render(tl, 13), footage(QStringLiteral("motion"), {}, 13)) <= 1);
    }

    // ---- Motion Blur ----

    void motionBlurSmearsAlongTheMotion()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        const QImage plain = footage(QStringLiteral("motion"), {}, 12);
        // Frame 12: the square covers x = 96..135 and moves 8 px per frame. A full-circle shutter
        // spreads every pixel over one frame of movement (8 px).
        const QImage blur = footage(QStringLiteral("motion"),
            {effect(t, fxid::TemporalMotionBlur, {{k::ShutterAngle, 360.0}, {k::Samples, 16.0}})},
            12);
        const int bg = grey(plain, 20, 90);
        qInfo("motion blur: ahead of the edge %d, behind %d, inside %d, far %d (background %d)",
            grey(blur, 138, 90), grey(blur, 93, 90), grey(blur, 115, 90), grey(blur, 20, 90), bg);
        QVERIFY(grey(blur, 138, 90) > bg + 25); // 2 px ahead of the leading edge: smeared in
        QVERIFY(grey(blur, 93, 90) > bg + 25); // 3 px behind the trailing edge
        QVERIFY(grey(blur, 138, 90) < 250); // but it is a blur, not a copy
        QVERIFY(grey(blur, 115, 90) > 245); // the inside stays solid
        QVERIFY(std::abs(grey(blur, 20, 90) - bg) <= 2); // still background stays still
        QVERIFY(std::abs(grey(blur, 150, 30) - grey(plain, 150, 30)) <= 2);
        // The streak is as long as the shutter says: half a shutter, half the reach.
        const QImage half = footage(QStringLiteral("motion"),
            {effect(t, fxid::TemporalMotionBlur, {{k::ShutterAngle, 90.0}, {k::Samples, 16.0}})},
            12);
        QVERIFY(grey(half, 138, 90) < grey(blur, 138, 90)); // a quarter frame: 2 px of reach
        QVERIFY(std::abs(grey(half, 143, 90) - bg) <= 2); // farther than the reach: untouched
        // No motion, no blur: a picture that does not move is unchanged by any shutter.
        const QImage still = footage(QStringLiteral("flicker"), {}, 12);
        QVERIFY(maxDifference(
                    footage(QStringLiteral("flicker"),
                        {effect(t, fxid::TemporalMotionBlur, {{k::ShutterAngle, 360.0}})}, 12),
                    still)
            <= 6); // a brightness flicker is not motion
    }

    // ---- Deflicker ----

    void deflickerFlattensTheBrightness()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        const QVector<double> before = frameMeans(QStringLiteral("flicker"), {}, 6, 30);
        const QVector<double> after = frameMeans(QStringLiteral("flicker"),
            {effect(t, fxid::Deflicker, {{k::FrameWindow, 2.0}, {k::MaxCorrection, 100.0}})}, 6,
            30);
        const double s0 = stddev(before), s1 = stddev(after);
        qInfo("deflicker: brightness spread %.2f -> %.2f levels", s0, s1);
        QVERIFY(s0 > 5.0); // the footage does flicker
        QVERIFY(s1 < 0.5 * s0);
        // Strength 50 % lands between.
        const QVector<double> halfway = frameMeans(QStringLiteral("flicker"),
            {effect(t, fxid::Deflicker,
                {{k::FrameWindow, 2.0}, {k::MaxCorrection, 100.0}, {k::Strength, 50.0}})},
            6, 30);
        QVERIFY(stddev(halfway) < s0 && stddev(halfway) > s1);
        // The limit holds the correction back: 5 % cannot flatten much.
        const QVector<double> limited = frameMeans(QStringLiteral("flicker"),
            {effect(t, fxid::Deflicker, {{k::FrameWindow, 2.0}, {k::MaxCorrection, 5.0}})}, 6, 30);
        QVERIFY(stddev(limited) > s1);
        // The picture is the same picture: the grid lines stay where they were.
        const QImage a = footage(QStringLiteral("flicker"), {effect(t, fxid::Deflicker)}, 15);
        QVERIFY(grey(a, 40, 20) > grey(a, 20, 20) + 10);
        // It matches the reference arithmetic: the gain pulls the frame's mean to the window mean.
        const QVector<double> plain = frameMeans(QStringLiteral("flicker"), {}, 13, 17);
        const double mean = (plain[0] + plain[1] + plain[2] + plain[3] + plain[4]) / 5;
        const double target = frameMeans(QStringLiteral("flicker"),
            {effect(t, fxid::Deflicker, {{k::FrameWindow, 2.0}, {k::MaxCorrection, 100.0}})}, 15,
            15)[0];
        qInfo("deflicker frame 15: %.2f, window mean %.2f", target, mean);
        QVERIFY(std::abs(target - mean) < 2.5);
    }

    // ---- Color Stabilizer ----

    void colorStabilizerHoldsTheReference()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        const QRect area(60, 60, 60, 60); // inside a grid cell: no grid lines
        const QImage ref = footage(QStringLiteral("tint"), {}, 0);
        const double refRed = meanChannel(ref, 0, area), refGreen = meanChannel(ref, 1, area);
        double worstPlain = 0, worstStable = 0;
        for (const int f : {3, 5, 8, 11, 14}) {
            const QImage plain = footage(QStringLiteral("tint"), {}, f);
            const QImage stable = footage(QStringLiteral("tint"),
                {effect(t, fxid::ColorStabilizer,
                    {{k::ReferenceFrame, 0.0}, {k::MaxCorrection, 300.0}},
                    {{k::Match, fxchoice::Both}})},
                f);
            worstPlain = std::max(worstPlain, std::abs(meanChannel(plain, 0, area) - refRed));
            worstStable = std::max(worstStable, std::abs(meanChannel(stable, 0, area) - refRed));
            QVERIFY(std::abs(meanChannel(stable, 1, area) - refGreen) < 3); // green was steady
        }
        qInfo(
            "color stabilizer: red off by %.1f levels without, %.1f with", worstPlain, worstStable);
        QVERIFY(worstPlain > 15);
        QVERIFY(worstStable < 4);
        // The reference frame itself is unchanged.
        QVERIFY(maxDifference(
                    footage(QStringLiteral("tint"), {effect(t, fxid::ColorStabilizer)}, 0), ref)
            <= 1);
        // White balance only keeps the brightness of the frame; exposure only keeps the colour
        // balance.
        const QImage f5 = footage(QStringLiteral("tint"), {}, 5);
        const QImage balance = footage(QStringLiteral("tint"),
            {effect(t, fxid::ColorStabilizer, {{k::MaxCorrection, 300.0}},
                {{k::Match, fxchoice::WhiteBalance}})},
            5);
        const QImage exposure = footage(QStringLiteral("tint"),
            {effect(t, fxid::ColorStabilizer, {{k::MaxCorrection, 300.0}},
                {{k::Match, fxchoice::Exposure}})},
            5);
        auto lum = [&](const QImage& img) {
            return qGray(QColor::fromRgbF(meanChannel(img, 0, area) / 255,
                meanChannel(img, 1, area) / 255, meanChannel(img, 2, area) / 255)
                             .rgb());
        };
        QVERIFY(std::abs(lum(balance) - lum(f5)) <= 3);
        auto ratio = [&](const QImage& img) {
            return meanChannel(img, 0, area) / meanChannel(img, 1, area);
        };
        QVERIFY(std::abs(ratio(balance) - refRed / refGreen)
            < 0.3 * std::abs(ratio(f5) - refRed / refGreen));
        QVERIFY(std::abs(meanChannel(exposure, 0, area) / meanChannel(exposure, 1, area)
                    - meanChannel(f5, 0, area) / meanChannel(f5, 1, area))
            < 0.05);
        // The Limit holds the correction back, the Strength scales it.
        const QImage limited = footage(QStringLiteral("tint"),
            {effect(t, fxid::ColorStabilizer, {{k::MaxCorrection, 2.0}})}, 5);
        QVERIFY(std::abs(meanChannel(limited, 0, area) - meanChannel(f5, 0, area)) < 6);
        const QImage half = footage(QStringLiteral("tint"),
            {effect(t, fxid::ColorStabilizer, {{k::Strength, 50.0}, {k::MaxCorrection, 300.0}})},
            5);
        const double mid = (meanChannel(f5, 0, area) + meanChannel(balance, 0, area)) / 2;
        QVERIFY(std::abs(meanChannel(half, 0, area) - mid) < 8);
    }

    // ---- Automatic Dirt Removal ----

    void dirtRemovalRemovesTheDotAndKeepsTheMovingEdge()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        const QImage plain = footage(QStringLiteral("dirt"), {}, 10);
        const int bg = grey(plain, 20, 20);
        QVERIFY(grey(plain, 203, 33) > 240); // the dot is there
        const QImage clean = footage(QStringLiteral("dirt"), {effect(t, fxid::DirtRemoval)}, 10);
        qInfo("dirt removal: dot %d -> %d (background %d)", grey(plain, 203, 33),
            grey(clean, 203, 33), bg);
        QVERIFY(grey(clean, 203, 33) <= bg + 3);
        QVERIFY(grey(clean, 262, 152) <= bg + 3); // the other dot too
        // Nothing else changed: the square (moving 6 px per frame, so its edges differ from the
        // frames before and after, but those two disagree) is as it was, edges included.
        int changed = 0;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const bool nearDot = (std::abs(x - 203) <= 6 && std::abs(y - 33) <= 6)
                    || (std::abs(x - 262) <= 6 && std::abs(y - 152) <= 6);
                if (!nearDot && std::abs(grey(clean, x, y) - grey(plain, x, y)) > 6)
                    ++changed;
            }
        }
        QCOMPARE(changed, 0);
        QVERIFY(grey(clean, 80, 90) > 240); // inside the square (x = 60..99)
        QVERIFY(grey(clean, 100, 90) <= bg + 3 || grey(clean, 100, 90) == grey(plain, 100, 90));
        // The frames before and after the dust frame are untouched.
        for (const int f : {9, 11})
            QVERIFY(
                maxDifference(footage(QStringLiteral("dirt"), {effect(t, fxid::DirtRemoval)}, f),
                    footage(QStringLiteral("dirt"), {}, f))
                <= 1);
        // Dirt Contrast above the dot's contrast leaves it; the Strength scales the repair.
        const QImage strict = footage(
            QStringLiteral("dirt"), {effect(t, fxid::DirtRemoval, {{k::Threshold, 100.0}})}, 10);
        QVERIFY(grey(strict, 203, 33) > 240);
        const QImage half = footage(
            QStringLiteral("dirt"), {effect(t, fxid::DirtRemoval, {{k::Amount, 50.0}})}, 10);
        QVERIFY(
            std::abs(grey(half, 203, 33) - (grey(plain, 203, 33) + grey(clean, 203, 33)) / 2) <= 3);
        // Grow: a larger Grow removes a ring around the dot that the dot's soft edge left.
        const QImage grown = footage(
            QStringLiteral("dirt"), {effect(t, fxid::DirtRemoval, {{k::Dilate, 6.0}})}, 10);
        QVERIFY(grey(grown, 203, 33) <= bg + 3);
    }

    void dirtRemovalMatchesTheReferenceArithmetic()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        const QImage f9 = footage(QStringLiteral("dirt"), {}, 9),
                     f10 = footage(QStringLiteral("dirt"), {}, 10),
                     f11 = footage(QStringLiteral("dirt"), {}, 11);
        const QImage clean = footage(QStringLiteral("dirt"),
            {effect(t, fxid::DirtRemoval,
                {{k::Dilate, 0.0}, {k::Threshold, 15.0}, {k::MotionThreshold, 8.0}})},
            10);
        auto rgba = [](const QImage& img, int x, int y) {
            const QRgb p = img.pixel(x, y);
            return fxmath::Rgba {qRed(p) / 255.f, qGreen(p) / 255.f, qBlue(p) / 255.f, 1.f};
        };
        int checked = 0, dirty = 0;
        for (int y = 0; y < kH; y += 3) {
            for (int x = 0; x < kW; x += 3) {
                const std::vector<fxmath::Rgba> n {rgba(f9, x, y), rgba(f11, x, y)};
                const fxmath::Rgba cur = rgba(f10, x, y);
                float spread = 0;
                const fxmath::Rgba median = fxmath::temporalMedian(n, &spread);
                const bool isDirt = fxmath::isDirt(cur, n, 0.15f, 0.08f);
                const fxmath::Rgba want = isDirt ? median : cur;
                const fxmath::Rgba got = rgba(clean, x, y);
                QVERIFY2(std::abs(got.r - want.r) * 255 <= 3 && std::abs(got.g - want.g) * 255 <= 3
                        && std::abs(got.b - want.b) * 255 <= 3,
                    qPrintable(QStringLiteral("%1,%2").arg(x).arg(y)));
                ++checked;
                dirty += isDirt ? 1 : 0;
            }
        }
        qInfo("dirt removal: %d pixels checked against the reference, %d dirt", checked, dirty);
        // Reference arithmetic on its own: a white dot between two equal frames.
        QVERIFY(fxmath::isDirt(
            {1, 1, 1, 1}, {{0.2f, 0.2f, 0.2f, 1}, {0.2f, 0.2f, 0.2f, 1}}, 0.15f, 0.08f));
        QVERIFY(!fxmath::isDirt({1, 1, 1, 1}, {{0.2f, 0.2f, 0.2f, 1}, {1.f, 1.f, 1.f, 1}}, 0.15f,
            0.08f)); // moving edge
        QVERIFY(!fxmath::isDirt({0.21f, 0.2f, 0.2f, 1},
            {{0.2f, 0.2f, 0.2f, 1}, {0.2f, 0.2f, 0.2f, 1}}, 0.15f, 0.08f)); // noise
    }

    void fiveFramesRemoveDustThatStaysTwoFrames()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        // The dot at (40, 140) shows in frames 20 and 21.
        for (const int f : {20, 21}) {
            const QImage plain = footage(QStringLiteral("dirt"), {}, f);
            const int bg = grey(plain, 20, 170);
            QVERIFY(grey(plain, 43, 143) > 240);
            const QImage three = footage(QStringLiteral("dirt"), {effect(t, fxid::DirtRemoval)}, f);
            const QImage five = footage(
                QStringLiteral("dirt"), {effect(t, fxid::DirtRemoval, {{k::FrameWindow, 2.0}})}, f);
            QVERIFY2(
                grey(three, 43, 143) > 240, "three frames cannot tell dust from a real change");
            QVERIFY2(grey(five, 43, 143) <= bg + 3, qPrintable(QString::number(f)));
        }
    }

    // ---- Dust Buster ----

    void dustBusterWorksInsideItsRegionOnly()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        const QImage plain = footage(QStringLiteral("dirt"), {}, 10);
        const int bg = grey(plain, 20, 20);
        // Dot A at x 200..205, y 30..35: centre (202.5, 32.5) = (42.5, -57.5) from the middle.
        auto buster = [&](double cx, double cy, const QString& shape, const QString& mode,
                          double feather = 0) {
            return footage(QStringLiteral("dirt"),
                {effect(t, fxid::DustBuster,
                    {{k::CenterX, cx}, {k::CenterY, cy}, {k::Width, 40.0}, {k::Height, 40.0},
                        {k::Feather, feather}},
                    {{k::Shape, shape}, {k::RegionMode, mode}})},
                10);
        };
        const QImage a = buster(42.5, -57.5, QStringLiteral("rectangle"), fxchoice::DetectedDirt);
        QVERIFY(grey(a, 203, 33) <= bg + 3); // inside the region: removed
        QVERIFY(grey(a, 262, 152) > 240); // dot C is outside: stays
        // Elsewhere nothing changes at all.
        QVERIFY(std::abs(grey(a, 80, 90) - grey(plain, 80, 90)) <= 1);
        // A region on the other dot swaps the result; an ellipse also covers the centre.
        const QImage c = buster(102.5, 62.5, QStringLiteral("rectangle"), fxchoice::DetectedDirt);
        QVERIFY(grey(c, 203, 33) > 240);
        QVERIFY(grey(c, 262, 152) <= bg + 3);
        QVERIFY(
            grey(buster(42.5, -57.5, QStringLiteral("ellipse"), fxchoice::DetectedDirt), 203, 33)
            <= bg + 3);
        // Feather ramps the repair in from the region's edge: far from the centre it is weaker.
        const QImage feathered
            = buster(42.5, -57.5, QStringLiteral("rectangle"), fxchoice::DetectedDirt, 30);
        QVERIFY(grey(feathered, 203, 33)
            > bg + 3); // the centre sits in the ramp (feather 30 > half size 20)
        // Whole region: everything inside is replaced by the temporal median, so the moving
        // square's edge inside the region would be replaced too. Put the region on the square.
        const QImage whole = footage(QStringLiteral("dirt"),
            {effect(t, fxid::DustBuster,
                {{k::CenterX, -75.0}, {k::CenterY, 0.0}, {k::Width, 30.0}, {k::Height, 30.0},
                    {k::Feather, 0.0}},
                {{k::RegionMode, fxchoice::WholeRegion}})},
            10);
        // Region centred at x = 85 (160 - 75), y = 90: the square covers 60..99 at frame 10, and
        // covers 54..93 / 66..105 around it, so the median of the neighbours is still the square
        // there; at the region's right edge (x = 99..100) the neighbours disagree.
        QVERIFY(std::abs(grey(whole, 85, 90) - grey(plain, 85, 90)) <= 2);
        // Whole region on the dust: replaced even where contrast is below the dirt threshold.
        const QImage all = footage(QStringLiteral("dirt"),
            {effect(t, fxid::DustBuster,
                {{k::CenterX, 42.5}, {k::CenterY, -57.5}, {k::Width, 40.0}, {k::Height, 40.0},
                    {k::Feather, 0.0}, {k::Threshold, 100.0}},
                {{k::RegionMode, fxchoice::WholeRegion}})},
            10);
        QVERIFY(grey(all, 203, 33) <= bg + 3);
        QVERIFY(grey(all, 262, 152) > 240);
    }

    // ---- Patch Replacer ----

    void patchReplacerCopiesARegion()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData base = emptyT();
        auto bars = [&](const QVector<EffectInstance>& effects) {
            TimelineData tl = emptyT();
            ClipData c;
            c.id = tl.allocateId();
            c.kind = ClipKind::Generator;
            c.generator = g::ColorBars;
            c.title = EffectRegistry::instance().defaultParams(g::ColorBars);
            c.title.setStatic(g::Style, QStringLiteral("full"));
            c.duration = 50;
            c.effects = effects;
            addClip(tl, TrackType::Video, 0, c);
            return render(tl, 0);
        };
        const QImage plain = bars({});
        // Copy the patch centred 100 px left of the middle onto the patch at the middle.
        const QImage patched = bars({effect(base, fxid::PatchReplacer,
            {{k::SourceX, -100.0}, {k::SourceY, 0.0}, {k::TargetX, 0.0}, {k::TargetY, 0.0},
                {k::Width, 60.0}, {k::Height, 60.0}, {k::Feather, 0.0}},
            {{k::Shape, QStringLiteral("rectangle")}})});
        QVERIFY(maxDifference(patched, plain) > 50); // it does something
        for (const auto& p :
            {QPoint(160, 90), QPoint(145, 75), QPoint(175, 105), QPoint(150, 100)}) {
            const QRgb want = plain.pixel(p.x() - 100, p.y());
            const QRgb got = patched.pixel(p.x(), p.y());
            QVERIFY2(std::abs(qRed(got) - qRed(want)) <= 3
                    && std::abs(qGreen(got) - qGreen(want)) <= 3
                    && std::abs(qBlue(got) - qBlue(want)) <= 3,
                qPrintable(QStringLiteral("%1,%2").arg(p.x()).arg(p.y())));
        }
        // Outside the patch the picture is as it was.
        QCOMPARE(patched.pixel(100, 90), plain.pixel(100, 90));
        QCOMPARE(patched.pixel(160, 150), plain.pixel(160, 150));
        QCOMPARE(patched.pixel(160, 20), plain.pixel(160, 20));
        QCOMPARE(patched.pixel(220, 90), plain.pixel(220, 90));
        // Blend 50 %: halfway between the picture and the patch.
        const QImage half = bars({effect(base, fxid::PatchReplacer,
            {{k::SourceX, -100.0}, {k::TargetX, 0.0}, {k::Width, 60.0}, {k::Height, 60.0},
                {k::Feather, 0.0}, {k::Opacity, 50.0}},
            {{k::Shape, QStringLiteral("rectangle")}})});
        const int mid = (qRed(plain.pixel(160, 90)) + qRed(patched.pixel(160, 90))) / 2;
        QVERIFY(std::abs(qRed(half.pixel(160, 90)) - mid) <= 3);
        // The ellipse leaves the corners of the box alone; the feather softens the edge.
        const QImage ellipse = bars({effect(base, fxid::PatchReplacer,
            {{k::SourceX, -100.0}, {k::TargetX, 0.0}, {k::Width, 60.0}, {k::Height, 60.0},
                {k::Feather, 0.0}},
            {{k::Shape, QStringLiteral("ellipse")}})});
        QCOMPARE(ellipse.pixel(131, 61 + 30 - 30 + 0),
            plain.pixel(131, 61)); // outside the ellipse, inside the box
        const QImage soft = bars({effect(base, fxid::PatchReplacer,
            {{k::SourceX, -100.0}, {k::TargetX, 0.0}, {k::Width, 60.0}, {k::Height, 60.0},
                {k::Feather, 20.0}},
            {{k::Shape, QStringLiteral("rectangle")}})});
        const int edgeStep = std::abs(qRed(soft.pixel(131, 90)) - qRed(plain.pixel(131, 90)));
        const int hardStep = std::abs(qRed(patched.pixel(131, 90)) - qRed(plain.pixel(131, 90)));
        QVERIFY(edgeStep < hardStep);
    }

    // ---- bounded window and missing frames (no GPU) ----

    void missingNeighboursUseTheNearestAvailableFrame()
    {
        struct FakeHistory final : LayerHistory
        {
            std::map<int64_t, RenderTarget*> available;
            int calls = 0;
            PooledTarget layerAt(int64_t f) override
            {
                ++calls;
                const auto it = available.find(f);
                return it == available.end() ? PooledTarget() : PooledTarget(nullptr, it->second);
            }
        };
        RenderTarget own, a, b;
        own.size = a.size = b.size = QSize(8, 8);
        FakeHistory history;
        history.available[11] = &a;
        history.available[14] = &b;
        EffectInput in;
        in.layer = &own;
        in.localFrame = 10;
        in.history = &history;
        // Wanted: 9, 11, 8, 12, 10 (itself), 14. Frame 10 is the layer; 11 and 14 render.
        fx::FrameFetch fetch(in, {9, 11, 8, 12, 10, 14});
        QCOMPARE(fetch.at(1), &a);
        QCOMPARE(fetch.at(5), &b);
        QCOMPARE(fetch.at(4), &own); // the frame itself is not rendered a second time
        QVERIFY(fetch.exact(1) && fetch.exact(4) && fetch.exact(5));
        QVERIFY(!fetch.exact(0) && !fetch.exact(2) && !fetch.exact(3));
        QCOMPARE(fetch.at(0), &own); // 9: frame 10 (the layer, one away) beats 11 (two away)
        QCOMPARE(fetch.at(3), &a); // 12: frame 11 is one away
        QCOMPARE(fetch.at(2), &own); // 8: frame 10 (two away) beats 11 (three away)
        QCOMPARE(history.calls, 5); // 9, 11, 8, 12, 14: not frame 10
        // Nothing renders (not decoded in time): every neighbour is the layer itself.
        FakeHistory none;
        in.history = &none;
        fx::FrameFetch lone(in, {9, 11});
        QCOMPARE(lone.at(0), &own);
        QCOMPARE(lone.at(1), &own);
        // No history at all (inside a composition): the same.
        in.history = nullptr;
        fx::FrameFetch bare(in, {9, 11});
        QCOMPARE(bare.at(0), &own);
        QVERIFY(!bare.exact(0));
    }

    // ---- reference arithmetic (no GPU) ----

    void referenceArithmetic()
    {
        using namespace fxmath;
        // Trails: Average weights sum to 1, Lighten keeps the layer at 1 and fades the trails.
        const std::vector<double> avg = trailWeights(3, 0.5, 1.0, true);
        QCOMPARE(avg.size(), size_t(4));
        QVERIFY(std::abs(avg[0] + avg[1] + avg[2] + avg[3] - 1.0) < 1e-9);
        QVERIFY(avg[1] > avg[2] && avg[2] > avg[3]);
        const std::vector<double> max = trailWeights(3, 0.5, 0.8, false);
        QCOMPARE(max[0], 1.0);
        QVERIFY(std::abs(max[1] - 0.4) < 1e-9 && std::abs(max[2] - 0.2) < 1e-9);
        // Smear: weights sum to 1 at any amount; the layer keeps what the amount leaves.
        for (const double amount : {0.0, 0.4, 1.0}) {
            const std::vector<double> w = smearWeights({-2, -1, 1, 2}, 0.3, amount);
            double sum = 0;
            for (const double x : w)
                sum += x;
            QVERIFY(std::abs(sum - 1.0) < 1e-9);
        }
        QCOMPARE(smearWeights({-1, 1}, 0.0, 1.0)[0], 1.0 / 3.0);
        // Deflicker: a bright frame gets a gain below 1, a dark one above; the limit and the
        // strength hold it.
        const std::vector<Rgba> means {
            {0.6f, 0.6f, 0.6f, 1}, {0.4f, 0.4f, 0.4f, 1}, {0.4f, 0.4f, 0.4f, 1}};
        const std::array<float, 3> bright = deflickerGain(means, false, 1.f, 1.f);
        QVERIFY(bright[0] < 1.f && std::abs(bright[0] - (1.4f / 3 / 0.6f)) < 1e-4f);
        QVERIFY(deflickerGain({means[1], means[0], means[2]}, false, 1.f, 1.f)[0] > 1.f);
        QVERIFY(
            std::abs(deflickerGain(means, false, 0.5f, 1.f)[0] - (1.f + 0.5f * (bright[0] - 1.f)))
            < 1e-5f);
        QVERIFY(deflickerGain(means, false, 1.f, 0.05f)[0] >= 1.f / 1.05f - 1e-5f);
        // Colour stabilizer: red off by 20 % gets a red gain only in Both and White Balance modes.
        const Rgba cur {0.6f, 0.4f, 0.4f, 1}, ref {0.5f, 0.4f, 0.4f, 1};
        const auto both = stabilizerGain(cur, ref, StabilizerMatch::Both, 1.f, 3.f);
        QVERIFY(std::abs(both[0] - 0.5f / 0.6f) < 1e-5f && std::abs(both[1] - 1.f) < 1e-5f);
        const auto exposure = stabilizerGain(cur, ref, StabilizerMatch::Exposure, 1.f, 3.f);
        QVERIFY(std::abs(exposure[0] - exposure[1]) < 1e-6f && exposure[0] < 1.f);
        const auto balance = stabilizerGain(cur, ref, StabilizerMatch::WhiteBalance, 1.f, 3.f);
        const float lumaBefore = 0.2126f * cur.r + 0.7152f * cur.g + 0.0722f * cur.b;
        const float lumaAfter = 0.2126f * cur.r * balance[0] + 0.7152f * cur.g * balance[1]
            + 0.0722f * cur.b * balance[2];
        QVERIFY(std::abs(lumaBefore - lumaAfter) < 1e-4f);
    }

    // ---- project ----

    void projectRoundTrip()
    {
        Project project;
        TimelineData& tl = project.timeline()->mutableData();
        tl = emptyT();
        ClipData fx = solidClip(tl, Qt::green, 0, 50);
        for (const Row& r : kEffects)
            fx.effects.append(effect(tl, r.id));
        fx.effects[0].params.values[k::Decay].setKey(0, 10.0);
        fx.effects[0].params.values[k::Decay].setKey(20, 70.0);
        fx.effects[0].params.setBase(k::Trails, 6);
        fx.effects[1].params.setStatic(k::SmearDirection, fxchoice::Behind);
        fx.effects[3].params.setStatic(k::FrameSource, fxchoice::Mix);
        fx.effects[4].params.setStatic(k::PerChannel, true);
        fx.effects[6].params.setStatic(k::RegionMode, fxchoice::WholeRegion);
        fx.effects[8].params.setBase(k::ReferenceFrame, 12);
        addClip(tl, TrackType::Video, 1, fx);
        project.timeline()->notifyChanged();
        const QString path = m_dir.filePath(QStringLiteral("effects5.kinoklip"));
        QString error;
        QVERIFY2(project.save(path, &error), qPrintable(error));
        Project loaded;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const ClipData& back = loaded.timeline()->data().videoTracks[1].clips[0];
        QCOMPARE(back.effects.size(), fx.effects.size());
        for (int i = 0; i < fx.effects.size(); ++i) {
            QCOMPARE(back.effects[i].type, fx.effects[i].type);
            QCOMPARE(back.effects[i].params.statics, fx.effects[i].params.statics);
            for (const QString& key : fx.effects[i].params.values.keys())
                QCOMPARE(back.effects[i].params.value(key, 15, -1),
                    fx.effects[i].params.value(key, 15, -1));
        }
        QVERIFY(back.effects[0].params.values.value(k::Decay).isAnimated());
        QCOMPARE(back.effects[0].params.value(k::Trails, 0, -1), 6.0);
        QCOMPARE(back.effects[3].params.staticValue(k::FrameSource).toString(), fxchoice::Mix);
        QCOMPARE(back.effects[8].params.value(k::ReferenceFrame, 0, -1), 12.0);
    }

    // ---- export and viewer give the same pixels ----

    void sameFrameSamePixelsWhateverWasRenderedBefore()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyT();
        const QVector<EffectInstance> stack {effect(t, fxid::DirtRemoval),
            effect(t, fxid::Deflicker), effect(t, fxid::MotionTrails, {{k::Trails, 3.0}}),
            effect(t, fxid::TemporalMotionBlur)};
        // Straight to frame 12 (a paused viewer) and after playing 8..12 in order (playback,
        // export): the decoders have been moved around, the picture must not depend on it.
        const QImage direct = footage(QStringLiteral("dirt"), stack, 12);
        for (int f = 8; f <= 11; ++f)
            footage(QStringLiteral("dirt"), stack, f);
        QCOMPARE(footage(QStringLiteral("dirt"), stack, 12), direct);
        footage(QStringLiteral("dirt"), stack, 30); // and after a jump away and back
        QCOMPARE(footage(QStringLiteral("dirt"), stack, 12), direct);
        QVERIFY(maxDifference(direct, footage(QStringLiteral("dirt"), {}, 12)) > 0);
    }

    // ---- contact sheet ----

    void contactSheet()
    {
        const QString out = qEnvironmentVariable("KK_EFFECT_SHEETS");
        if (out.isEmpty() || !gpuReady())
            QSKIP("KK_EFFECT_SHEETS not set, or no OpenGL");
        QDir().mkpath(out);
        TimelineData t = emptyT();
        struct Strip
        {
            QString label;
            QString media;
            QVector<EffectInstance> effects;
            QVector<int> frames;
        };
        const QVector<Strip> strips {
            {QStringLiteral("Motion Trails: before"), QStringLiteral("motion"), {},
                {10, 12, 14, 16}},
            {QStringLiteral("Motion Trails: after"), QStringLiteral("motion"),
                {effect(t, fxid::MotionTrails, {{k::Trails, 5.0}})}, {10, 12, 14, 16}},
            {QStringLiteral("Smear: after"), QStringLiteral("motion"),
                {effect(t, fxid::Smear, {{k::Length, 2.0}})}, {10, 12, 14, 16}},
            {QStringLiteral("Motion Blur: after"), QStringLiteral("motion"),
                {effect(t, fxid::TemporalMotionBlur, {{k::ShutterAngle, 360.0}})},
                {10, 12, 14, 16}},
            {QStringLiteral("Dust: before"), QStringLiteral("dirt"), {}, {9, 10, 11, 20}},
            {QStringLiteral("Automatic Dirt Removal: after"), QStringLiteral("dirt"),
                {effect(t, fxid::DirtRemoval, {{k::FrameWindow, 2.0}})}, {9, 10, 11, 20}},
            {QStringLiteral("Dust Buster (region on one dot): after"), QStringLiteral("dirt"),
                {effect(t, fxid::DustBuster,
                    {{k::CenterX, 42.5}, {k::CenterY, -57.5}, {k::Width, 60.0},
                        {k::Height, 60.0}})},
                {9, 10, 11, 20}},
            {QStringLiteral("Deflicker: before"), QStringLiteral("flicker"), {}, {10, 11, 12, 13}},
            {QStringLiteral("Deflicker: after"), QStringLiteral("flicker"),
                {effect(t, fxid::Deflicker, {{k::FrameWindow, 2.0}, {k::MaxCorrection, 100.0}})},
                {10, 11, 12, 13}},
            {QStringLiteral("Color Stabilizer: before"), QStringLiteral("tint"), {}, {2, 5, 8, 11}},
            {QStringLiteral("Color Stabilizer: after"), QStringLiteral("tint"),
                {effect(t, fxid::ColorStabilizer, {{k::MaxCorrection, 300.0}})}, {2, 5, 8, 11}},
        };
        constexpr int tileW = 320, tileH = 180, label = 22, cols = 4;
        QImage sheet(cols * tileW, int(strips.size() + 2) * (tileH + label), QImage::Format_RGB32);
        sheet.fill(QColor(30, 30, 34));
        QPainter p(&sheet);
        p.setPen(Qt::white);
        int row = 0;
        auto drawRow = [&](const QString& text, const QVector<QImage>& images) {
            const int y = row++ * (tileH + label);
            p.drawText(QRect(6, y, sheet.width() - 6, label), Qt::AlignVCenter, text);
            for (int i = 0; i < images.size(); ++i)
                p.drawImage(i * tileW, y + label, images[i]);
        };
        for (const Strip& s : strips) {
            QVector<QImage> images;
            for (const int f : s.frames)
                images.append(footage(s.media, s.effects, f));
            QStringList numbers;
            for (const int f : s.frames)
                numbers << QString::number(f);
            drawRow(s.label + QStringLiteral(" (frames %1)").arg(numbers.join(QLatin1Char(' '))),
                images);
        }
        // Patch Replacer on colour bars.
        {
            TimelineData tl = emptyT();
            ClipData c;
            c.id = tl.allocateId();
            c.kind = ClipKind::Generator;
            c.generator = g::ColorBars;
            c.title = EffectRegistry::instance().defaultParams(g::ColorBars);
            c.duration = 50;
            addClip(tl, TrackType::Video, 0, c);
            const QImage before = render(tl, 0);
            tl.videoTracks[0].clips[0].effects = {effect(tl, fxid::PatchReplacer,
                {{k::SourceX, -100.0}, {k::TargetX, 0.0}, {k::Width, 80.0}, {k::Height, 80.0},
                    {k::Feather, 10.0}})};
            drawRow(QStringLiteral("Patch Replacer: before, after"), {before, render(tl, 0)});
        }
        // Frame Replacer: the frame replaced by the next one.
        const QImage frameBefore = footage(QStringLiteral("motion"), {}, 12);
        const QImage frameAfter = footage(QStringLiteral("motion"),
            {effect(t, fxid::FrameReplacer, {}, {{k::FrameSource, fxchoice::Next}})}, 12);
        drawRow(QStringLiteral("Frame Replacer (next): frame 12 before, after"),
            {frameBefore, frameAfter});
        p.end();
        QVERIFY(sheet.save(out + QStringLiteral("/effects-5.png")));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestEffects5>(argc, argv);
}

#include "test_effects5.moc"
