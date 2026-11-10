// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the tracking effects of the 1.2 gap list (docs/behavior-spec.md section 70): Match Move,
// Surface Tracker and Warper. The footage is made with the ffmpeg command line at the start
// (lossless): a white square with a dark marker that moves 4 px per frame on dark grey; the
// truth is measured on the footage itself. GPU cases skip without OpenGL. Also writes the contact
// sheet trackers.png when KK_EFFECT_SHEETS is set.
#include "RenderTestUtil.h"
#include "compositing/tracking/EffectTrackJob.h"
#include "core/EffectRegistry.h"
#include "core/EffectTrack.h"
#include "core/MatchMove.h"
#include "compositing/tracking/Homography.h"
#include "compositing/tracking/MeshTracker.h"
#include "core/Serialization.h"
#include "core/SurfaceTracker.h"
#include "core/VideoEffectKeys.h"
#include "render/Compositor.h"
#include "render/GlEffects.h"
#include "render/TimelineEval.h"

#include <QDir>
#include <QEventLoop>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;
using namespace kk::render;
using namespace kk::tracking;

namespace {

namespace k = fxkeys;

constexpr int kW = 320, kH = 180;

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

/// Centre of the pixels of @p img that satisfy @p test, NaN when there are none.
template<typename F>
QPointF centroid(const QImage& img, F test)
{
    double sx = 0, sy = 0;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (test(img.pixel(x, y))) {
                sx += x + 0.5;
                sy += y + 0.5;
                ++n;
            }
        }
    }
    return n ? QPointF(sx / n, sy / n) : QPointF(qQNaN(), qQNaN());
}

QPointF whiteCentre(const QImage& img)
{
    return centroid(img, [](QRgb p) { return qRed(p) > 200 && qGreen(p) > 200 && qBlue(p) > 200; });
}

QPointF redCentre(const QImage& img)
{
    return centroid(img, [](QRgb p) { return qRed(p) > 200 && qGreen(p) < 90 && qBlue(p) < 90; });
}

double dist(QPointF a, QPointF b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

} // namespace

class TestTrackers : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    QTemporaryDir m_dir;
    QString m_footageError;

    bool gpuReady() const { return m_gpu && m_gpu->isGpu(); }

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

    /// The footage alone: V1 holds the clip "patch" (40 frames).
    TimelineData footageTimeline(const QVector<EffectInstance>& effects = {})
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = mediaClip(tl, QStringLiteral("patch"), 0, 40, 0, 0);
        clip.effects = effects;
        addClip(tl, TrackType::Video, 0, std::move(clip));
        return tl;
    }

    /// Runs the plan's tracking job on the compositor and returns the samples.
    std::vector<TrackSample> runJob(const EffectTrackPlan& plan, QString* error = nullptr)
    {
        TrackJob job = plan.job;
        Compositor* gpu = m_gpu.get();
        const auto timeline = plan.timeline;
        const QSize size = plan.renderSize;
        job.source = [gpu, timeline, size](int64_t frame) {
            RenderRequest r;
            r.timeline = timeline;
            r.frame = frame;
            r.outSize = size;
            r.outputs = OutputImage;
            r.subtitles = false;
            return gpu->renderAsync(std::move(r));
        };
        TrackerRunner runner;
        QSignalSpy done(&runner, &TrackerRunner::finished);
        if (!runner.start(job))
            return {};
        if (!done.wait(120000))
            return {};
        if (error)
            *error = done.first().at(1).toString();
        return runner.samples();
    }

    /// Applies the track of the effect (forward then backward from @p playhead).
    bool trackBoth(TimelineData& tl, quint64 clipId, quint64 effectId, int64_t playhead)
    {
        for (const bool forward : {true, false}) {
            QString why;
            const auto plan = planEffectTrack(tl, clipId, effectId, playhead, forward, &why);
            if (!plan) {
                qWarning("plan: %s", qPrintable(why));
                return false;
            }
            QString error;
            const auto samples = runJob(*plan, &error);
            if (samples.size() < 2) {
                qWarning("tracking: %s", qPrintable(error));
                return false;
            }
            const auto table = tableFromSamples(*plan, samples);
            for (TrackData& track : tl.videoTracks) {
                for (ClipData& c : track.clips) {
                    if (c.id == clipId)
                        storeEffectTrack(c, effectId, *table);
                }
            }
        }
        return true;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_gpu = std::make_unique<Compositor>();
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
        const bool ok = makeFootage(QStringLiteral("patch"),
            {QStringLiteral("color=c=0x303030:s=320x180:r=25"),
                QStringLiteral("color=c=white:s=40x40:r=25"),
                QStringLiteral("color=c=0x202020:s=12x12:r=25")},
            QStringLiteral("[0][1]overlay=x='n*4':y=60[a];[a][2]overlay=x='n*4+8':y=68[out]"));
        auto map = std::make_shared<MediaPathMap>();
        map->insert(QStringLiteral("patch"), m_dir.filePath(QStringLiteral("patch.mkv")));
        const QString tex = QStringLiteral(
            "128+40*sin(0.35*X+0.2*Y)+30*sin(0.21*X-0.43*Y)+25*sin(0.67*X+0.11*Y)"
            "+20*sin(0.13*X+0.71*Y)");
        auto texture = [&](const QString& x, const QString& y) {
            QString t = tex;
            t.replace(QLatin1Char('X'), QStringLiteral("(") + x + QStringLiteral(")"));
            t.replace(QLatin1Char('Y'), QStringLiteral("(") + y + QStringLiteral(")"));
            return QStringLiteral("[0]geq=lum='%1':cb=128:cr=128[out]").arg(t);
        };
        const QString blank = QStringLiteral("color=c=black:s=320x180:r=25");
        const bool ok2 = makeFootage(QStringLiteral("plane"), {blank},
            texture(QStringLiteral("X-3*N"), QStringLiteral("Y-2*N")));
        const bool ok3 = makeFootage(QStringLiteral("bend"), {blank},
            texture(QStringLiteral("X"), QStringLiteral("Y-N*0.15*sin(0.04*X)")));
        map->insert(QStringLiteral("plane"), m_dir.filePath(QStringLiteral("plane.mkv")));
        map->insert(QStringLiteral("bend"), m_dir.filePath(QStringLiteral("bend.mkv")));
        m_gpu->setMediaPaths(map);
        if (!ok || !ok2 || !ok3)
            qWarning("footage: %s", qPrintable(m_footageError));
    }
    void cleanupTestCase() { m_gpu.reset(); }

    void footageWasMade() { QVERIFY2(m_footageError.isEmpty(), qPrintable(m_footageError)); }

    // ---- the stored track ----

    void trackTableRoundTrip()
    {
        const efftrack::Table t(QStringLiteral("sig|1"), 1.5, 2, {1.0, 1.04, 1.08},
            {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f});
        const QString text = efftrack::encode(t);
        const auto back = efftrack::decode(text);
        QVERIFY(back);
        QVERIFY(*back == t);
        float v[2] = {};
        back->at(1.02, v);
        QVERIFY(std::abs(v[0] - 0.2f) < 1e-5 && std::abs(v[1] - 0.3f) < 1e-5);
        back->at(0.0, v); // held before the first key
        QCOMPARE(v[0], 0.1f);
        back->at(9.0, v); // held after the last
        QCOMPARE(v[0], 0.5f);
        // Damaged text is refused.
        QVERIFY(!efftrack::decode(QString()));
        QVERIFY(!efftrack::decode(QStringLiteral("kkt1;xx;1;2;3;zz")));
        QVERIFY(!efftrack::decode(text.left(text.size() - 12)));
        QVERIFY(!efftrack::decode(QStringLiteral("other;a;b;c;d;e")));
    }

    void trackTablesMerge()
    {
        const efftrack::Table a(QStringLiteral("s"), 2.0, 1, {1.0, 2.0, 3.0}, {1.f, 2.f, 3.f});
        const efftrack::Table b(QStringLiteral("s"), 2.0, 1, {3.0, 4.0}, {30.f, 40.f});
        const auto m = efftrack::merged(&a, b);
        QCOMPARE(m->keys(), 4);
        QCOMPARE(m->values()[2], 30.f); // the new table wins at equal times
        const efftrack::Table other(QStringLiteral("t"), 2.0, 1, {5.0}, {5.f});
        QCOMPARE(efftrack::merged(&a, other)->keys(), 1); // another signature replaces
    }

    // ---- Match Move: maths ----

    void descriptorIsRegistered()
    {
        const EffectDescriptor* d = EffectRegistry::instance().find(QLatin1String(fxid::MatchMove));
        QVERIFY(d);
        QCOMPARE(d->name, QStringLiteral("Match Move"));
        QCOMPARE(d->group, QLatin1String(effectgroups::Transform));
        const ParamDescriptor* data = d->param(k::TrackData);
        QVERIFY(data && data->hidden);
        QVERIFY(!d->param(k::RegionX)->animatable);
        QVERIFY(d->param(k::OffsetX)->animatable);
    }

    void motionOfAPointAndARegion()
    {
        const float ref[2] = {0.5f, 0.5f}, cur[2] = {0.6f, 0.4f};
        const matchmove::Motion m = matchmove::motionBetween(ref, cur, 2, QSizeF(200, 100));
        QVERIFY(std::abs(m.dx - 20) < 1e-4 && std::abs(m.dy - 10) < 1e-4);
        QCOMPARE(m.pivotX, 100.0);
        QCOMPARE(m.pivotY, 50.0);
        // A square (in a 100 x 100 picture) turned 90 degrees clockwise and doubled in size.
        const float r[8] = {0.4f, 0.6f, 0.6f, 0.6f, 0.6f, 0.4f, 0.4f, 0.4f};
        const float c[8] = {0.7f, 0.7f, 0.7f, 0.3f, 0.3f, 0.3f, 0.3f, 0.7f};
        const matchmove::Motion q = matchmove::motionBetween(r, c, 8, QSizeF(100, 100));
        QVERIFY(std::abs(q.angleDeg - 90.0) < 1e-3);
        QVERIFY(std::abs(q.scale - 2.0) < 1e-3);
        QVERIFY(std::abs(q.dx) < 1e-3 && std::abs(q.dy) < 1e-3);
    }

    void trackIsKeyedBySourceTimeAndStaleWhenTheRegionChanges()
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = mediaClip(tl, QStringLiteral("patch"), 100, 20, 1000000, 0);
        clip.speed = 2.0;
        EffectInstance fx = effect(tl, fxid::MatchMove);
        // The point moves right by 0.1 picture widths per source second; reference at second 1.
        const efftrack::Table table(matchmove::signature(fx.params), 1.0, 2,
            {0.0, 1.0, 2.0, 3.0}, {0.4f, 0.5f, 0.5f, 0.5f, 0.6f, 0.5f, 0.7f, 0.5f});
        fx.params.setStatic(k::TrackData, efftrack::encode(table));
        clip.effects = {fx};
        QVERIFY(matchmove::hasActive(clip));
        // Local frame 10 at speed 2 on a 25 fps timeline: source second 1 + 10 * 2 / 25 = 1.8.
        const QTransform t = matchmove::transform(clip, 10, QSizeF(kW, kH), Rational {25, 1});
        QVERIFY(std::abs(t.dx() - 25.6) < 0.05);
        QVERIFY(std::abs(t.dy()) < 1e-6);
        // A changed region makes the stored track stale: no movement, "track again".
        clip.effects[0].params.setBase(k::RegionX, 60.0);
        QVERIFY(!matchmove::hasActive(clip));
        QVERIFY(matchmove::needsTracking(clip.effects[0]));
        QVERIFY(matchmove::transform(clip, 10, QSizeF(kW, kH), Rational {25, 1}).isIdentity());
        clip.effects[0].params.setBase(k::RegionX, 50.0);
        QVERIFY(matchmove::hasActive(clip));
    }

    void trackSurvivesTheProjectFile()
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = mediaClip(tl, QStringLiteral("patch"), 0, 40, 0, 0);
        EffectInstance fx = effect(tl, fxid::MatchMove, {{k::OffsetX, 12.0}});
        const efftrack::Table table(matchmove::signature(fx.params), 0.0, 2, {0.0, 0.5, 1.0},
            {0.5f, 0.5f, 0.55f, 0.5f, 0.6f, 0.45f});
        fx.params.setStatic(k::TrackData, efftrack::encode(table));
        clip.effects = {fx};
        addClip(tl, TrackType::Video, 0, clip);
        TimelineData loaded;
        QString error;
        QVERIFY2(serialization::timelineFromJson(serialization::timelineToJson(tl), loaded, &error),
            qPrintable(error));
        const ClipData& back = loaded.videoTracks[0].clips[0];
        QVERIFY(matchmove::hasActive(back));
        const QTransform a = matchmove::transform(clip, 20, QSizeF(kW, kH), Rational {25, 1});
        const QTransform b = matchmove::transform(back, 20, QSizeF(kW, kH), Rational {25, 1});
        QCOMPARE(a, b);
        QVERIFY(!a.isIdentity());
    }

    // ---- Match Move: tracking footage ----

    void followsAMovingPatch()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        // V1 the footage, V2 a small red patch that is put on the square at frame 10.
        TimelineData tl = footageTimeline();
        const QPointF at10 = whiteCentre(render(tl, 10));
        QVERIFY(!std::isnan(at10.x()));
        ClipData red = solidClip(tl, QColor(255, 0, 0), 0, 40);
        red.transform.setBase(keys::ZoomX, 0.1);
        red.transform.setBase(keys::ZoomY, 0.1);
        red.transform.setBase(keys::PosX, at10.x() - kW / 2.0);
        red.transform.setBase(keys::PosY, at10.y() - kH / 2.0);
        EffectInstance fx = effect(tl, fxid::MatchMove,
            {{k::RegionX, at10.x() / kW * 100}, {k::RegionY, at10.y() / kH * 100},
                {k::RegionSize, 14.0}},
            {{k::TrackSource, fxchoice::TrackBelow}});
        red.effects = {fx};
        const quint64 clipId = red.id, effectId = fx.id;
        addClip(tl, TrackType::Video, 1, std::move(red));
        QVERIFY(trackBoth(tl, clipId, effectId, 10));
        double worst = 0;
        for (int f = 0; f < 40; ++f) {
            const QImage img = render(tl, f);
            const QPointF patch = redCentre(img);
            QVERIFY2(!std::isnan(patch.x()), qPrintable(QString::number(f)));
            // The white square is hidden under the red patch only in the middle; its edges stay.
            const QPointF truth = whiteCentre(render(footageTimeline(), f));
            worst = std::max(worst, dist(patch, truth));
        }
        qInfo("Match Move (position): worst distance %.3f px", worst);
        QVERIFY2(worst <= 1.0, qPrintable(QString::number(worst)));
    }

    void planarVariantFollowsToo()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData tl = footageTimeline();
        const QPointF at10 = whiteCentre(render(tl, 10));
        ClipData red = solidClip(tl, QColor(255, 0, 0), 0, 40);
        red.transform.setBase(keys::ZoomX, 0.1);
        red.transform.setBase(keys::ZoomY, 0.1);
        red.transform.setBase(keys::PosX, at10.x() - kW / 2.0);
        red.transform.setBase(keys::PosY, at10.y() - kH / 2.0);
        EffectInstance fx = effect(tl, fxid::MatchMove,
            {{k::RegionX, at10.x() / kW * 100}, {k::RegionY, at10.y() / kH * 100},
                {k::RegionSize, 16.0}},
            {{k::TrackSource, fxchoice::TrackBelow}, {k::TrackKind, fxchoice::KindPlanar}});
        red.effects = {fx};
        const quint64 clipId = red.id, effectId = fx.id;
        addClip(tl, TrackType::Video, 1, std::move(red));
        QVERIFY(trackBoth(tl, clipId, effectId, 10));
        double worst = 0;
        for (const int f : {0, 5, 15, 25, 39}) {
            const QPointF truth = whiteCentre(render(footageTimeline(), f));
            worst = std::max(worst, dist(redCentre(render(tl, f)), truth));
        }
        qInfo("Match Move (planar): worst distance %.3f px", worst);
        QVERIFY2(worst <= 1.5, qPrintable(QString::number(worst)));
    }

    void holdTheTrackInPlaceOnTheClipItself()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        // The footage tracked in its own picture and held: the square stays where it is at 10.
        TimelineData tl = footageTimeline();
        const QPointF at10 = whiteCentre(render(tl, 10));
        EffectInstance fx = effect(tl, fxid::MatchMove,
            {{k::RegionX, at10.x() / kW * 100}, {k::RegionY, at10.y() / kH * 100},
                {k::RegionSize, 14.0}},
            {{k::TrackDirection, fxchoice::DirectionLock}});
        tl.videoTracks[0].clips[0].effects = {fx};
        const quint64 clipId = tl.videoTracks[0].clips[0].id;
        QVERIFY(trackBoth(tl, clipId, fx.id, 10));
        double worst = 0;
        for (const int f : {0, 5, 20, 30}) {
            const QPointF c = whiteCentre(render(tl, f));
            if (!std::isnan(c.x()))
                worst = std::max(worst, dist(c, at10));
        }
        qInfo("Match Move (hold): worst distance %.3f px", worst);
        QVERIFY2(worst <= 1.5, qPrintable(QString::number(worst)));
    }

    // ---- Surface Tracker ----

    void surfaceDescriptor()
    {
        const EffectDescriptor* d
            = EffectRegistry::instance().find(QLatin1String(fxid::SurfaceTracker));
        QVERIFY(d);
        QCOMPARE(d->name, QStringLiteral("Surface Tracker"));
        QCOMPARE(d->group, QLatin1String(effectgroups::Transform));
        QVERIFY(d->param(k::TrackData)->hidden);
        QCOMPARE(int(d->param(k::GridDensity)->def), 4);
        QVERIFY(!d->param(k::GridDensity)->animatable);
        // The signature follows the inputs of the tracker only.
        ParamSet p = EffectRegistry::instance().defaultParams(QLatin1String(fxid::SurfaceTracker));
        const QString a = surface::signature(p);
        p.setBase(k::Strength, 50.0);
        p.setStatic(k::SurfaceMode, fxchoice::SurfaceWarp);
        QCOMPARE(surface::signature(p), a);
        p.setBase(k::GridDensity, 5.0);
        QVERIFY(surface::signature(p) != a);
    }

    void smoothingBlendsTowardsTheNeighbours()
    {
        // 3 x 3 grid: the middle point is off by 6 px, the others agree: smoothing pulls it in.
        std::vector<Vec2> bend(9);
        std::vector<bool> known(9, true);
        bend[4] = {6, 0};
        smoothBend(bend, known, 3, 3, 0.0);
        QCOMPARE(bend[4].x, 6.0);
        smoothBend(bend, known, 3, 3, 1.0);
        QVERIFY(bend[4].x < 1.0);
        // An unknown point takes the mean of its known neighbours.
        std::vector<Vec2> b2(9);
        std::vector<bool> k2(9, true);
        b2[1] = {10, 0};
        b2[3] = {20, 0};
        k2[0] = false;
        smoothBend(b2, k2, 3, 3, 0.0);
        QCOMPARE(b2[0].x, 15.0);
    }

private:
    /// 40 frames of the texture plane footage "plane"/"bend" as gray images.
    GrayImage grayOf(const QString& media, int frame)
    {
        TimelineData tl = emptyTimeline(kW, kH);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, media, 0, 40, 0, 0));
        return grayFromImage(render(tl, frame));
    }

    /// Mean distance of @p mesh from the truth @p truth(i), pixels.
    template<typename F>
    double meshError(const std::vector<Vec2>& mesh, F truth)
    {
        double sum = 0;
        for (size_t i = 0; i < mesh.size(); ++i) {
            const Vec2 t = truth(i);
            sum += std::hypot(mesh[i].x - t.x, mesh[i].y - t.y);
        }
        return sum / double(mesh.size());
    }

private slots:
    void meshFollowsAPlanarMotion()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        // The textured plane moves 3 px right and 2 px down per frame.
        const int n = 4;
        std::vector<Vec2> rest;
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c)
                rest.push_back({70.0 + 30 * c, 50.0 + 30 * r});
        MeshSettings s;
        s.cols = s.rows = n;
        s.patchSize = 21;
        s.searchSize = 41;
        MeshTracker tracker(s);
        QVERIFY(tracker.reset(grayOf(QStringLiteral("plane"), 0), rest));
        double worst = 0;
        for (int f = 1; f <= 10; ++f) { // the grid stays well inside the picture until then
            const MeshResult r = tracker.track(grayOf(QStringLiteral("plane"), f));
            QVERIFY2(r.ok, qPrintable(QString::number(f)));
            worst = std::max(worst, meshError(r.points, [&](size_t i) {
                return Vec2 {rest[i].x + 3.0 * f, rest[i].y + 2.0 * f};
            }));
        }
        qInfo("Surface Tracker (plane): worst mean error %.3f px", worst);
        QVERIFY2(worst <= 1.0, qPrintable(QString::number(worst)));
    }

    void meshFollowsABend()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        // The picture bends: a feature at (x, y) is at y + N * 0.15 * sin(0.04 x) in frame N.
        const int n = 5;
        std::vector<Vec2> rest;
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c)
                rest.push_back({60.0 + 50 * c, 40.0 + 25 * r});
        auto truth = [&](size_t i, int f) {
            return Vec2 {rest[i].x, rest[i].y + f * 0.15 * std::sin(0.04 * rest[i].x)};
        };
        MeshSettings s;
        s.cols = s.rows = n;
        s.patchSize = 21;
        s.searchSize = 41;
        s.smoothing = 0.0;
        MeshTracker tracker(s);
        QVERIFY(tracker.reset(grayOf(QStringLiteral("bend"), 0), rest));
        MeshResult last;
        for (int f = 1; f <= 24; ++f) {
            last = tracker.track(grayOf(QStringLiteral("bend"), f));
            QVERIFY2(last.ok, qPrintable(QString::number(f)));
            if (f % 4 == 0)
                qInfo("bend frame %d: good %d corr %.2f error %.2f", f, last.good, last.correlation,
                    meshError(last.points, [&](size_t i) { return truth(i, f); }));
        }
        const double meshOnly = meshError(last.points, [&](size_t i) { return truth(i, 24); });
        // A single perspective map (what the planar tracker can do) cannot follow the bend.
        std::vector<Vec2> to;
        for (size_t i = 0; i < rest.size(); ++i)
            to.push_back(truth(i, 24));
        const auto fit = homographyFromPoints(rest, to);
        QVERIFY(fit);
        const double planarOnly = meshError(
            [&] {
                std::vector<Vec2> m;
                for (const Vec2& p : rest)
                    m.push_back(fit->map(p));
                return m;
            }(),
            [&](size_t i) { return truth(i, 24); });
        qInfo("Surface Tracker (bend): mesh %.3f px, best single plane %.3f px", meshOnly, planarOnly);
        QVERIFY2(meshOnly <= 1.2, qPrintable(QString::number(meshOnly)));
        QVERIFY(meshOnly < planarOnly);
    }


private:
    /// A stored 2 x 2 mesh track whose corners all move by (@p du, 0) at source second 1.
    static EffectInstance shiftedMesh(TimelineData& tl, double du, const QString& mode)
    {
        EffectInstance fx = effect(tl, fxid::SurfaceTracker, {{k::GridDensity, 2.0}},
            {{k::SurfaceMode, mode}});
        const float u0 = 0.2f, u1 = 0.8f, v0 = 0.8f, v1 = 0.2f;
        const float d = float(du);
        const efftrack::Table table(surface::signature(fx.params), 0.0, 8, {0.0, 1.0},
            {u0, v0, u1, v0, u0, v1, u1, v1, u0 + d, v0, u1 + d, v0, u0 + d, v1, u1 + d, v1});
        fx.params.setStatic(k::TrackData, efftrack::encode(table));
        return fx;
    }

private slots:
    void surfacePinMovesTheClip()
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = mediaClip(tl, QStringLiteral("patch"), 0, 40, 0, 0);
        clip.effects = {shiftedMesh(tl, 0.1, fxchoice::SurfacePin)};
        QVERIFY(surface::hasActivePin(clip));
        const QTransform t = surface::pinTransform(clip, 25, QSizeF(kW, kH), Rational {25, 1});
        QVERIFY(std::abs(t.dx() - 0.1 * kW) < 0.05);
        QVERIFY(std::abs(t.dy()) < 0.05);
        clip.effects[0].params.setStatic(k::SurfaceMode, fxchoice::SurfaceWarp);
        QVERIFY(!surface::hasActivePin(clip));
        clip.effects[0].params.setBase(k::GridDensity, 3.0); // the inputs changed: stale
        clip.effects[0].params.setStatic(k::SurfaceMode, fxchoice::SurfacePin);
        QVERIFY(!surface::hasActivePin(clip));
        QVERIFY(surface::needsTracking(clip.effects[0]));
    }

    void surfaceWarpBendsThePicture()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData tl = footageTimeline();
        const QPointF before = whiteCentre(render(tl, 25));
        tl.videoTracks[0].clips[0].effects = {shiftedMesh(tl, 0.1, fxchoice::SurfaceWarp)};
        const QPointF after = whiteCentre(render(tl, 25));
        QVERIFY2(std::abs(after.x() - before.x() - 0.1 * kW) < 3.0,
            qPrintable(QStringLiteral("%1 -> %2").arg(before.x()).arg(after.x())));
        QVERIFY(std::abs(after.y() - before.y()) < 2.0);
        // Pin mode leaves the pixels to the transform: the plain render shows no warp pass.
        tl.videoTracks[0].clips[0].effects[0].params.setStatic(k::SurfaceMode, fxchoice::SurfacePin);
        const QPointF pinned = whiteCentre(render(tl, 25));
        QVERIFY2(std::abs(pinned.x() - before.x() - 0.1 * kW) < 3.0, "pin");
    }

    void refusals()
    {
        TimelineData tl = footageTimeline();
        ClipData title = titleClip(tl, QStringLiteral("x"), 0, 40);
        EffectInstance fx
            = effect(tl, fxid::MatchMove, {}, {{k::TrackSource, fxchoice::TrackBelow}});
        title.effects = {fx};
        const quint64 id = title.id;
        addClip(tl, TrackType::Video, 1, std::move(title));
        QString why;
        QVERIFY(planEffectTrack(tl, id, fx.id, 100, true, &why) == std::nullopt);
        QVERIFY(!why.isEmpty()); // the playhead is outside the clip
        QVERIFY(planEffectTrack(tl, id, fx.id + 99, 5, true, &why) == std::nullopt);
        QVERIFY(planEffectTrack(tl, 9999, fx.id, 5, true, &why) == std::nullopt);
        // Nothing below the first track.
        TimelineData one = footageTimeline(
            {effect(tl, fxid::MatchMove, {}, {{k::TrackSource, fxchoice::TrackBelow}})});
        QVERIFY(planEffectTrack(one, one.videoTracks[0].clips[0].id,
                    one.videoTracks[0].clips[0].effects[0].id, 5, true, &why)
            == std::nullopt);
        QVERIFY(why.contains(QStringLiteral("below")));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestTrackers>(argc, argv);
}

#include "test_trackers.moc"
