// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Tracker node (docs/compositing-page.md, "Tracker"): the node descriptor, the
// Match Move / Stabilize maths, the rendered picture following a stored track (GPU and CPU
// backends), the TrackerRunner on a real composition rendered by the compositor, and the
// project-file round trip of the track data.
#include "../render/RenderTestUtil.h"
#include "compositing/tracking/TrackStore.h"
#include "compositing/tracking/TrackerRunner.h"
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"
#include "render/CompMath.h"
#include "render/Compositor.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>

#include <memory>

using namespace kk;
using namespace kk::test;
using namespace kk::tracking;
namespace co = kk::compops;
namespace compmath = kk::render::compmath;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

TimelineData compTimeline(const Composition& comp)
{
    TimelineData tl = emptyTimeline(kW, kH);
    ClipData c = solidClip(tl, QColor(0, 0, 0), 0, 50);
    c.composition = comp;
    addClip(tl, TrackType::Video, 0, c);
    return tl;
}

quint64 mediaIn(const Composition& c)
{
    for (const CompNode& n : c.nodes) {
        if (n.type == nodetypes::MediaIn)
            return n.id;
    }
    return 0;
}

/// A white 20 x 20 box at (90..110, 90..110) timeline px, then a Tracker, then MediaOut.
Composition boxWithTracker(quint64* tracker)
{
    Composition c = co::defaultComposition();
    const co::AddResult bg = co::addNodeAfter(c, nodetypes::Background, mediaIn(c));
    c.node(bg.node)->params.setStatic(nodekeys::Color, QColor(255, 255, 255));
    // Keep only the box (edges in timeline px).
    const co::AddResult crop = co::addNodeAfter(c, nodetypes::Crop, bg.merge ? bg.merge : bg.node);
    CompNode* cn = c.node(crop.node);
    cn->params.setBase(nodekeys::Left, 90);
    cn->params.setBase(nodekeys::Right, kW - 110);
    cn->params.setBase(nodekeys::Top, 90);
    cn->params.setBase(nodekeys::Bottom, kH - 110);
    const co::AddResult t = co::addNodeAfter(c, nodetypes::Tracker, crop.node);
    *tracker = t.node;
    return c;
}

/// Moving noise (Transform with a keyframed centre) then a Tracker. @p mover gets the id of
/// the Transform. The noise moves by (@p dx, @p dy) pixels per frame (dy: down on screen).
Composition movingNoise(double dx, double dy, quint64* mover, quint64* tracker)
{
    Composition c = co::defaultComposition();
    const quint64 noise = co::addNode(c, nodetypes::FastNoise, {});
    c.node(noise)->params.setBase(nodekeys::Scale, 60.0);
    c.node(noise)->params.setBase(nodekeys::Detail, 3.0);
    const quint64 xf = co::addNode(c, nodetypes::Transform, {});
    co::connect(c, noise, 0, xf, nodeports::Input);
    for (int f = 0; f <= 20; f += 20) {
        c.node(xf)->params.animated(nodekeys::CenterX, 0.5).setKey(f, 0.5 + dx * f / kW);
        c.node(xf)->params.animated(nodekeys::CenterY, 0.5).setKey(f, 0.5 - dy * f / kH);
    }
    const quint64 t = co::addNode(c, nodetypes::Tracker, {});
    co::connect(c, xf, 0, t, nodeports::Input);
    co::connect(c, t, 0, c.mediaOutId, nodeports::Input);
    *mover = xf;
    *tracker = t;
    return c;
}

/// Centre of the white pixels (alpha-weighted) in @p img, or (-1, -1).
QPointF whiteCentre(const QImage& img)
{
    double sx = 0, sy = 0, n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qRed(img.pixel(x, y)) > 128 && qAlpha(img.pixel(x, y)) > 128) {
                sx += x;
                sy += y;
                ++n;
            }
    return n > 0 ? QPointF(sx / n, sy / n) : QPointF(-1, -1);
}

} // namespace

class TestTrackerNode : public QObject
{
    Q_OBJECT
    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    QImage render(Compositor* c, const TimelineData& tl, int64_t frame)
    {
        return c->renderImage(snap(tl), frame);
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
    }
    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    void descriptor()
    {
        const NodeDescriptor* d = NodeRegistry::instance().find(nodetypes::Tracker);
        QVERIFY(d);
        QCOMPARE(d->mainInput, nodeports::Input);
        QVERIFY(d->input(nodeports::Source));
        QVERIFY(d->input(nodeports::Source)->analysisOnly);
        QVERIFY(!d->input(nodeports::Source)->required);
        QVERIFY(d->param(nodekeys::CenterX) && d->param(nodekeys::CenterX)->animatable);
        QVERIFY(d->param(nodekeys::CenterY) && d->param(nodekeys::CenterY)->animatable);
        QVERIFY(d->param(nodekeys::PatternSize) && d->param(nodekeys::SearchSize));
        QCOMPARE(d->controls, ViewerControls::Tracker);
    }

    void trackingSourceIsNotEvaluated()
    {
        quint64 mover = 0, tracker = 0;
        Composition c = movingNoise(1, 0, &mover, &tracker);
        const quint64 bg = co::addNode(c, nodetypes::Background, {});
        co::connect(c, bg, 0, tracker, nodeports::Source);
        const QVector<quint64> order = co::evaluationOrder(c, c.mediaOutId);
        QVERIFY(order.contains(tracker));
        QVERIFY(!order.contains(bg));
    }

    void matchMoveMatrix()
    {
        ParamSet p = NodeRegistry::instance().defaultParams(nodetypes::Tracker);
        QVERIFY(compmath::trackerMatrix(p, 5, QSizeF(kW, kH)).isIdentity()); // no track yet
        p.setBase(nodekeys::RefFrame, 2);
        p.animated(nodekeys::CenterX, 0.5).setKey(2, 0.25);
        p.animated(nodekeys::CenterY, 0.5).setKey(2, 0.5);
        p.animated(nodekeys::CenterX, 0.5).setKey(12, 0.25 + 40.0 / kW);
        p.animated(nodekeys::CenterY, 0.5).setKey(12, 0.5 - 18.0 / kH); // 18 px down
        QVERIFY(compmath::trackerMatrix(p, 2, QSizeF(kW, kH)).isIdentity());
        const QTransform m = compmath::trackerMatrix(p, 12, QSizeF(kW, kH));
        QCOMPARE(m.dx(), 40.0);
        QCOMPARE(m.dy(), 18.0);
        p.setStatic(nodekeys::TrackMode, trackmodes::Stabilize);
        const QTransform s = compmath::trackerMatrix(p, 12, QSizeF(kW, kH));
        QCOMPARE(s.dx(), -40.0);
        QCOMPARE(s.dy(), -18.0);
    }

    void matchMoveFollowsTheTrackInTheRender()
    {
        quint64 tracker = 0;
        Composition c = boxWithTracker(&tracker);
        // Track: the point is at (0.5, 0.5) on frame 0 and moves 20 px right and 18 px down by
        // frame 10 (v is bottom-up).
        std::vector<TrackSample> samples;
        for (int f = 0; f <= 10; ++f)
            samples.push_back({f, 0.5 + 20.0 * f / 10 / kW, 0.5 - 18.0 * f / 10 / kH, 1.0});
        QVERIFY(storeTrack(*c.node(tracker), samples, 0));
        QCOMPARE(c.node(tracker)->params.value(nodekeys::RefFrame, 0, -1), 0.0);
        const TimelineData tl = compTimeline(c);
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            const QPointF at0 = whiteCentre(render(comp, tl, 0));
            QVERIFY2(std::abs(at0.x() - 99.5) < 1 && std::abs(at0.y() - 99.5) < 1,
                qPrintable(QStringLiteral("frame 0 box at %1,%2").arg(at0.x()).arg(at0.y())));
            const QPointF at10 = whiteCentre(render(comp, tl, 10));
            QVERIFY2(
                std::abs(at10.x() - (at0.x() + 20)) < 1 && std::abs(at10.y() - (at0.y() + 18)) < 1,
                qPrintable(QStringLiteral("frame 10 box at %1,%2").arg(at10.x()).arg(at10.y())));
            const QPointF at5 = whiteCentre(render(comp, tl, 5));
            QVERIFY2(
                std::abs(at5.x() - (at0.x() + 10)) < 1 && std::abs(at5.y() - (at0.y() + 9)) < 1,
                qPrintable(QStringLiteral("frame 5 box at %1,%2").arg(at5.x()).arg(at5.y())));
        }
    }

    void halfPixelMatchMoveBlendsEdgesLikeTheCpu()
    {
        // A 0.5 px track must give blended edge pixels (bilinear sampling), on the GPU as on the
        // CPU; a whole-pixel jump would leave the edge hard.
        quint64 tracker = 0;
        Composition c = boxWithTracker(&tracker);
        QVERIFY(storeTrack(*c.node(tracker), {{0, 0.5, 0.5, 1}, {10, 0.5 + 0.5 / kW, 0.5, 1}}, 0));
        const TimelineData tl = compTimeline(c);
        const QImage gpu = render(m_gpu.get(), tl, 10);
        const QImage cpu = render(m_cpu.get(), tl, 10);
        // Row 100, the box's left edge is at x = 90: pixel 90 is half covered after a 0.5 px move.
        const int g = qRed(gpu.pixel(90, 100)), k = qRed(cpu.pixel(90, 100));
        QVERIFY2(g > 90 && g < 170, qPrintable(QStringLiteral("gpu edge %1").arg(g)));
        QVERIFY2(std::abs(g - k) <= 16, qPrintable(QStringLiteral("gpu %1 cpu %2").arg(g).arg(k)));
    }

    void stabilizeCancelsTheTrack()
    {
        quint64 tracker = 0;
        Composition c = boxWithTracker(&tracker);
        c.node(tracker)->params.setStatic(nodekeys::TrackMode, trackmodes::Stabilize);
        std::vector<TrackSample> samples {{0, 0.5, 0.5, 1}, {10, 0.5 + 20.0 / kW, 0.5, 1}};
        QVERIFY(storeTrack(*c.node(tracker), samples, 0));
        const TimelineData tl = compTimeline(c);
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            const QPointF at0 = whiteCentre(render(comp, tl, 0));
            const QPointF at10 = whiteCentre(render(comp, tl, 10));
            QVERIFY2(std::abs(at10.x() - (at0.x() - 20)) < 1 && std::abs(at10.y() - at0.y()) < 1,
                qPrintable(QStringLiteral("box at %1,%2").arg(at10.x()).arg(at10.y())));
        }
    }

    void storeTrackKeepsTheReferenceOfAnExistingTrack()
    {
        CompNode node;
        node.type = nodetypes::Tracker;
        node.params = NodeRegistry::instance().defaultParams(nodetypes::Tracker);
        QVERIFY(!storeTrack(node, {}, 0));
        QVERIFY(storeTrack(node, {{4, 0.5, 0.5, 1}, {5, 0.51, 0.5, 0.9}}, 4));
        QCOMPARE(node.params.value(nodekeys::RefFrame, 0, -1), 4.0);
        // A second track from another frame replaces the keys it covers, keeps the reference.
        QVERIFY(storeTrack(node, {{5, 0.52, 0.5, 1}, {6, 0.53, 0.5, 0.9}}, 5));
        QCOMPARE(node.params.value(nodekeys::RefFrame, 0, -1), 4.0);
        QCOMPARE(node.params.values.value(nodekeys::CenterX).keys.size(), 3);
        QCOMPARE(node.params.value(nodekeys::CenterX, 5, 0), 0.52);
        clearTrack(node, 5);
        QVERIFY(!node.params.values.value(nodekeys::CenterX).isAnimated());
        QCOMPARE(node.params.value(nodekeys::CenterX, 0, 0), 0.52);
    }

    void trackDataSurvivesSaveAndLoad()
    {
        quint64 tracker = 0;
        Composition c = boxWithTracker(&tracker);
        std::vector<TrackSample> samples;
        for (int f = 3; f <= 30; ++f)
            samples.push_back({f, 0.4 + 0.00371 * f, 0.6 - 0.00213 * f, 0.9});
        QVERIFY(storeTrack(*c.node(tracker), samples, 3));
        c.node(tracker)->params.setStatic(nodekeys::TrackMode, trackmodes::Stabilize);
        c.node(tracker)->params.setBase(nodekeys::PatternSize, 64);
        // The composition as JSON text, exactly as the project file stores it.
        const QByteArray text
            = QJsonDocument(serialization::compositionToJson(c)).toJson(QJsonDocument::Compact);
        const Composition back
            = serialization::compositionFromJson(QJsonDocument::fromJson(text).object());
        QCOMPARE(back, c);
        const CompNode* n = back.node(tracker);
        QVERIFY(n);
        QCOMPARE(n->params.values.value(nodekeys::CenterX).keys.size(), 28);
        QCOMPARE(n->params.value(nodekeys::CenterX, 17, 0), 0.4 + 0.00371 * 17);
        QCOMPARE(n->params.value(nodekeys::RefFrame, 0, -1), 3.0);
        QCOMPARE(n->params.staticValue(nodekeys::TrackMode).toString(), trackmodes::Stabilize);
        // And inside a clip (the project file).
        ClipData clip;
        clip.id = 7;
        clip.kind = ClipKind::Solid;
        clip.duration = 50;
        clip.composition = c;
        QCOMPARE(serialization::clipFromJson(serialization::clipToJson(clip)), clip);
    }

    void runnerFollowsMovingNoise()
    {
        // The noise moves by (1.5, 0.75) px per frame (sub-pixel; both backends sample
        // bilinearly); the runner
        // renders the Transform node through the compositor, like the Tracker panel does with the
        // playback engine.
        quint64 mover = 0, tracker = 0;
        const Composition c = movingNoise(1.5, 0.75, &mover, &tracker);
        const TimelineData tl = compTimeline(c);
        const quint64 clipId = tl.videoTracks[0].clips[0].id;
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            TrackJob job;
            job.firstFrame = 0;
            job.lastFrame = 12;
            job.startU = 0.5;
            job.startV = 0.5;
            job.patternFraction = 48.0 / kW;
            job.searchFraction = 120.0 / kW;
            const auto shared = snap(tl);
            job.source = [comp, shared, clipId, mover](int64_t f) {
                return comp->renderCompositionNode(
                    shared, clipId, mover, f, {}, Quality::Full, OutputImage);
            };
            TrackerRunner runner;
            QSignalSpy done(&runner, &TrackerRunner::finished);
            QSignalSpy progress(&runner, &TrackerRunner::progress);
            QVERIFY(runner.start(job));
            QVERIFY(runner.isRunning());
            QVERIFY(done.wait(60000));
            QCOMPARE(done.first().first().value<TrackOutcome>(), TrackOutcome::Finished);
            QVERIFY(!runner.isRunning());
            QCOMPARE(int(runner.samples().size()), 13);
            QCOMPARE(progress.last().at(0).toInt(), 13);
            for (const TrackSample& s : runner.samples()) {
                const double x = 159.5 + 1.5 * s.frame, y = 89.5 + 0.75 * s.frame; // px, y down
                const double px = s.u * kW - 0.5, py = (1.0 - s.v) * kH - 0.5;
                QVERIFY2(std::hypot(px - x, py - y) < 0.3,
                    qPrintable(QStringLiteral("frame %1: tracked %2,%3 expected %4,%5")
                                   .arg(s.frame)
                                   .arg(px)
                                   .arg(py)
                                   .arg(x)
                                   .arg(y)));
            }
        }
    }

    void runnerTracksBackwardsAndCanBeStopped()
    {
        quint64 mover = 0, tracker = 0;
        const Composition c = movingNoise(1.0, 0.0, &mover, &tracker);
        const TimelineData tl = compTimeline(c);
        const quint64 clipId = tl.videoTracks[0].clips[0].id;
        Compositor* comp = m_cpu.get();
        const auto shared = snap(tl);
        TrackJob job;
        job.firstFrame = 15; // the point is where the noise has moved to by frame 15
        job.lastFrame = 0;
        job.startU = (159.5 + 15 + 0.5) / kW;
        job.startV = 0.5;
        job.patternFraction = 48.0 / kW;
        job.searchFraction = 120.0 / kW;
        job.source = [comp, shared, clipId, mover](int64_t f) {
            return comp->renderCompositionNode(
                shared, clipId, mover, f, {}, Quality::Full, OutputImage);
        };
        TrackerRunner runner;
        QSignalSpy done(&runner, &TrackerRunner::finished);
        QVERIFY(runner.start(job));
        QVERIFY(done.wait(60000));
        QCOMPARE(done.first().first().value<TrackOutcome>(), TrackOutcome::Finished);
        QCOMPARE(int(runner.samples().size()), 16);
        QCOMPARE(runner.samples().front().frame, int64_t(15));
        QCOMPARE(runner.samples().back().frame, int64_t(0));
        QVERIFY(std::abs(runner.samples().back().u * kW - 0.5 - 159.5) < 0.5);

        // Stop: the run ends at once and reports Stopped; late results are dropped.
        done.clear();
        job.firstFrame = 0;
        job.lastFrame = 19;
        job.startU = 0.5;
        QVERIFY(runner.start(job));
        runner.stop();
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.first().first().value<TrackOutcome>(), TrackOutcome::Stopped);
        QVERIFY(!runner.isRunning());
        QTest::qWait(300); // a step in flight must not report after the stop
        QCOMPARE(done.count(), 1);
    }

    void runnerReportsAFlatPattern()
    {
        // Nothing but the black solid clip: no detail to follow.
        const Composition c = co::defaultComposition();
        const TimelineData tl = compTimeline(c);
        const quint64 clipId = tl.videoTracks[0].clips[0].id;
        Compositor* comp = m_cpu.get();
        const auto shared = snap(tl);
        TrackJob job;
        job.firstFrame = 0;
        job.lastFrame = 5;
        job.source = [comp, shared, clipId](int64_t f) {
            return comp->renderCompositionNode(
                shared, clipId, 0, f, {}, Quality::Full, OutputImage);
        };
        TrackerRunner runner;
        QSignalSpy done(&runner, &TrackerRunner::finished);
        QVERIFY(runner.start(job));
        QVERIFY(done.wait(30000));
        QCOMPARE(done.first().first().value<TrackOutcome>(), TrackOutcome::Lost);
        QVERIFY(!done.first().at(1).toString().isEmpty());
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestTrackerNode>(argc, argv);
}
#include "test_trackernode.moc"
