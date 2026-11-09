// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Planar Tracker node and the track link (docs/compositing-page.md, "Planar
// Tracker"): the node descriptor, the Corner Pin / Unsteady / Steady maths, the pinned picture
// in the render (GPU and CPU backends), the TrackerRunner following a turning, scaling plane in
// a real composition, storing / clearing the corners, the project-file round trip, and
// connecting another node's Center to a track (point or planar).
#include "../render/RenderTestUtil.h"
#include "compositing/tracking/TrackLink.h"
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

#include <cmath>
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

/// Sets the corners of planar node @p node at @p frame from timeline pixels (y down), as keys.
void setCornersPx(CompNode& node, int64_t frame, const std::array<QPointF, 4>& px)
{
    for (int i = 0; i < 4; ++i) {
        node.params.animated(nodekeys::planarCorner(i, false), 0.5)
            .setKey(frame, px[size_t(i)].x() / kW, Interp::Linear);
        node.params.animated(nodekeys::planarCorner(i, true), 0.5)
            .setKey(frame, 1.0 - px[size_t(i)].y() / kH, Interp::Linear);
    }
}

TrackSample sampleOf(int64_t frame, const std::array<QPointF, 4>& px)
{
    TrackSample s;
    s.frame = frame;
    s.planar = true;
    for (size_t i = 0; i < 4; ++i) {
        s.corners[2 * i] = px[i].x() / kW;
        s.corners[2 * i + 1] = 1.0 - px[i].y() / kH;
    }
    return s;
}

/// A white background, then a Planar Tracker (corner pin) on it, then MediaOut.
Composition whiteWithPlanar(quint64* planar)
{
    Composition c = co::defaultComposition();
    const co::AddResult bg = co::addNodeAfter(c, nodetypes::Background, mediaIn(c));
    c.node(bg.node)->params.setStatic(nodekeys::Color, QColor(255, 255, 255));
    *planar = co::addNodeAfter(c, nodetypes::PlanarTracker, bg.node).node;
    return c;
}

/// Moving noise (a Transform with keyed centre, size and angle) then a Planar Tracker.
Composition turningNoise(quint64* mover, quint64* planar)
{
    Composition c = co::defaultComposition();
    const quint64 noise = co::addNode(c, nodetypes::FastNoise, {});
    c.node(noise)->params.setBase(nodekeys::Scale, 60.0);
    c.node(noise)->params.setBase(nodekeys::Detail, 3.0);
    const quint64 xf = co::addNode(c, nodetypes::Transform, {});
    co::connect(c, noise, 0, xf, nodeports::Input);
    for (const int f : {0, 12}) {
        CompNode* n = c.node(xf);
        n->params.animated(nodekeys::CenterX, 0.5).setKey(f, 0.5 + 6.0 * f / kW);
        n->params.animated(nodekeys::CenterY, 0.5).setKey(f, 0.5 - 3.0 * f / kH);
        n->params.animated(nodekeys::Angle, 0.0).setKey(f, 0.5 * f);
        n->params.animated(nodekeys::Size, 1.0).setKey(f, 1.0 + 0.006 * f);
    }
    const quint64 p = co::addNode(c, nodetypes::PlanarTracker, {});
    co::connect(c, xf, 0, p, nodeports::Input);
    co::connect(c, p, 0, c.mediaOutId, nodeports::Input);
    *mover = xf;
    *planar = p;
    return c;
}

bool close(QPointF a, QPointF b, double tol)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y()) <= tol;
}

} // namespace

class TestPlanarTrackerNode : public QObject
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
        const NodeDescriptor* d = NodeRegistry::instance().find(nodetypes::PlanarTracker);
        QVERIFY(d);
        QCOMPARE(d->mainInput, nodeports::Input);
        QVERIFY(d->input(nodeports::Source) && d->input(nodeports::Source)->analysisOnly);
        QCOMPARE(d->controls, ViewerControls::Planar);
        for (int i = 0; i < 4; ++i) {
            for (const bool y : {false, true}) {
                const ParamDescriptor* p = d->param(nodekeys::planarCorner(i, y));
                QVERIFY(p && p->animatable);
            }
        }
        QVERIFY(d->param(nodekeys::TrackMode) && d->param(nodekeys::RefFrame));
        QVERIFY(!d->param(nodekeys::RefFrame)->animatable);
        // The default region is the middle half of the frame.
        const ParamSet p = NodeRegistry::instance().defaultParams(nodetypes::PlanarTracker);
        QCOMPARE(p.value(nodekeys::planarCorner(0, false), 0, -1), 0.25);
        QCOMPARE(p.value(nodekeys::planarCorner(0, true), 0, -1), 0.75);
        QCOMPARE(p.value(nodekeys::planarCorner(2, false), 0, -1), 0.75);
        QCOMPARE(p.value(nodekeys::planarCorner(2, true), 0, -1), 0.25);
        QCOMPARE(p.staticValue(nodekeys::TrackMode).toString(), trackmodes::CornerPin);
        // Evaluation does not pull the tracking source.
        quint64 planar = 0;
        Composition c = whiteWithPlanar(&planar);
        const quint64 other = co::addNode(c, nodetypes::Background, {});
        co::connect(c, other, 0, planar, nodeports::Source);
        QVERIFY(!co::evaluationOrder(c, c.mediaOutId).contains(other));
    }

    void cornerPinMapsTheFrameOntoTheCorners()
    {
        ParamSet p = NodeRegistry::instance().defaultParams(nodetypes::PlanarTracker);
        const QSizeF size(kW, kH);
        // The untouched default: the frame goes to its middle half.
        QTransform m = compmath::planarMatrix(p, 0, size);
        QVERIFY(close(m.map(QPointF(0, 0)), QPointF(kW * 0.25, kH * 0.25), 1e-6));
        QVERIFY(close(m.map(QPointF(kW, kH)), QPointF(kW * 0.75, kH * 0.75), 1e-6));
        // A perspective quad (corner positions in timeline px, y down).
        CompNode n;
        n.type = nodetypes::PlanarTracker;
        n.params = p;
        const std::array<QPointF, 4> quad {
            QPointF(60, 40), QPointF(260, 20), QPointF(300, 160), QPointF(30, 150)};
        setCornersPx(n, 0, quad);
        m = compmath::planarMatrix(n.params, 0, size);
        QVERIFY(!m.isAffine());
        const QPointF frameCorner[4] {
            QPointF(0, 0), QPointF(kW, 0), QPointF(kW, kH), QPointF(0, kH)};
        for (int i = 0; i < 4; ++i)
            QVERIFY(close(m.map(frameCorner[i]), quad[size_t(i)], 1e-4));
        const std::array<QPointF, 4> read = compmath::planarCorners(n.params, 0, size);
        QVERIFY(close(read[1], quad[1], 1e-9));
    }

    void unsteadyFollowsAndSteadyCancelsTheMovement()
    {
        CompNode n;
        n.type = nodetypes::PlanarTracker;
        n.params = NodeRegistry::instance().defaultParams(nodetypes::PlanarTracker);
        const QSizeF size(kW, kH);
        const std::array<QPointF, 4> ref {
            QPointF(100, 50), QPointF(220, 50), QPointF(220, 130), QPointF(100, 130)};
        // At frame 10 the plane is turned by 12 degrees, 1.25 times larger, moved by (30, -15).
        const double a = 12.0 * M_PI / 180, s = 1.25;
        std::array<QPointF, 4> now;
        for (size_t i = 0; i < 4; ++i) {
            const double x = ref[i].x(), y = ref[i].y();
            now[i] = QPointF(s * (std::cos(a) * x - std::sin(a) * y) + 30,
                s * (std::sin(a) * x + std::cos(a) * y) - 15);
        }
        setCornersPx(n, 2, ref);
        setCornersPx(n, 10, now);
        n.params.setBase(nodekeys::RefFrame, 2);
        n.params.setStatic(nodekeys::TrackMode, trackmodes::MatchMove);
        // At the reference frame nothing moves.
        QVERIFY(close(
            compmath::planarMatrix(n.params, 2, size).map(QPointF(7, 9)), QPointF(7, 9), 1e-6));
        const QTransform unsteady = compmath::planarMatrix(n.params, 10, size);
        for (size_t i = 0; i < 4; ++i)
            QVERIFY(close(unsteady.map(ref[i]), now[i], 1e-4)); // the input rides on the plane
        n.params.setStatic(nodekeys::TrackMode, trackmodes::Stabilize);
        const QTransform steady = compmath::planarMatrix(n.params, 10, size);
        for (size_t i = 0; i < 4; ++i)
            QVERIFY(close(steady.map(now[i]), ref[i], 1e-4)); // the plane is held where it was
        QVERIFY(close((unsteady * steady).map(QPointF(33, 44)), QPointF(33, 44), 1e-6));
    }

    void cornerPinShowsTheInputInsideTheQuadOnly()
    {
        quint64 planar = 0;
        Composition c = whiteWithPlanar(&planar);
        const std::array<QPointF, 4> quad {
            QPointF(60, 40), QPointF(260, 20), QPointF(300, 160), QPointF(30, 150)};
        setCornersPx(*c.node(planar), 0, quad);
        const TimelineData tl = compTimeline(c);
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            const QImage img = render(comp, tl, 0);
            QVERIFY(qRed(img.pixel(160, 90)) > 240); // inside
            QVERIFY(qRed(img.pixel(70, 60)) > 240); // inside near the left edge
            QVERIFY(qRed(img.pixel(290, 150)) > 240); // inside near the bottom right corner
            QVERIFY(qRed(img.pixel(10, 10)) < 20); // outside
            QVERIFY(qRed(img.pixel(310, 40)) < 20);
            QVERIFY(qRed(img.pixel(20, 170)) < 20);
            // The right edge runs from (260, 20) to (300, 160): at y = 90 it is at x = 280.
            QVERIFY(qRed(img.pixel(272, 90)) > 200);
            QVERIFY(qRed(img.pixel(288, 90)) < 60);
        }
    }

    void cornerPinFollowsTheTrackedCorners()
    {
        quint64 planar = 0;
        Composition c = whiteWithPlanar(&planar);
        const std::array<QPointF, 4> a {
            QPointF(60, 40), QPointF(160, 40), QPointF(160, 120), QPointF(60, 120)};
        std::array<QPointF, 4> b = a;
        for (QPointF& p : b)
            p += QPointF(100, 30);
        QVERIFY(storeTrack(*c.node(planar), {sampleOf(0, a), sampleOf(10, b)}, 0));
        const TimelineData tl = compTimeline(c);
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            QVERIFY(qRed(render(comp, tl, 0).pixel(110, 80)) > 240);
            QVERIFY(qRed(render(comp, tl, 0).pixel(210, 110)) < 20);
            QVERIFY(qRed(render(comp, tl, 10).pixel(210, 110)) > 240);
            QVERIFY(qRed(render(comp, tl, 10).pixel(110, 80)) < 20);
            QVERIFY(qRed(render(comp, tl, 5).pixel(160, 95)) > 240); // half way
        }
    }

    void gpuAndCpuAgreeOnAPerspectivePin()
    {
        quint64 planar = 0;
        Composition c = whiteWithPlanar(&planar);
        setCornersPx(*c.node(planar), 0,
            {QPointF(60, 40), QPointF(260, 20), QPointF(300, 160), QPointF(30, 150)});
        const TimelineData tl = compTimeline(c);
        const QImage gpu = render(m_gpu.get(), tl, 0), cpu = render(m_cpu.get(), tl, 0);
        int differing = 0;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x)
                differing += std::abs(qRed(gpu.pixel(x, y)) - qRed(cpu.pixel(x, y))) > 64;
        }
        // Only antialiased edge pixels may differ.
        QVERIFY2(differing < 4 * (kW + kH), qPrintable(QString::number(differing)));
    }

    void storeAndClearTheCorners()
    {
        CompNode node;
        node.type = nodetypes::PlanarTracker;
        node.params = NodeRegistry::instance().defaultParams(nodetypes::PlanarTracker);
        QVERIFY(!storeTrack(node, {}, 0));
        QVERIFY(!storeTrack(node, {TrackSample {3, 0.5, 0.5, 1.0}}, 3)); // a point sample: refused
        const std::array<QPointF, 4> a {
            QPointF(60, 40), QPointF(160, 40), QPointF(160, 120), QPointF(60, 120)};
        QVERIFY(storeTrack(node, {sampleOf(4, a), sampleOf(5, a)}, 4));
        QCOMPARE(node.params.value(nodekeys::RefFrame, 0, -1), 4.0);
        QCOMPARE(node.params.values.value(nodekeys::planarCorner(3, true)).keys.size(), 2);
        QVERIFY(std::abs(node.params.value(nodekeys::planarCorner(1, false), 5, 0) - 160.0 / kW)
            < 1e-9);
        // A later track keeps the reference frame and replaces keys it covers.
        QVERIFY(storeTrack(node, {sampleOf(5, a), sampleOf(6, a)}, 5));
        QCOMPARE(node.params.value(nodekeys::RefFrame, 0, -1), 4.0);
        QCOMPARE(node.params.values.value(nodekeys::planarCorner(0, false)).keys.size(), 3);
        clearTrack(node, 5);
        for (int i = 0; i < 4; ++i) {
            QVERIFY(!node.params.values.value(nodekeys::planarCorner(i, false)).isAnimated());
            QVERIFY(!node.params.values.value(nodekeys::planarCorner(i, true)).isAnimated());
        }
        QVERIFY(
            std::abs(node.params.value(nodekeys::planarCorner(2, true), 0, 0) - (1.0 - 120.0 / kH))
            < 1e-9);
    }

    void planarTrackDataSurvivesSaveAndLoad()
    {
        quint64 planar = 0;
        Composition c = whiteWithPlanar(&planar);
        std::vector<TrackSample> samples;
        for (int f = 3; f <= 20; ++f) {
            const double dx = 2.5 * f, dy = -1.25 * f;
            samples.push_back(sampleOf(f,
                {QPointF(60 + dx, 40 + dy), QPointF(160 + dx * 1.01, 42 + dy),
                    QPointF(158 + dx, 120 + dy * 0.99), QPointF(62 + dx, 118 + dy)}));
        }
        QVERIFY(storeTrack(*c.node(planar), samples, 3));
        c.node(planar)->params.setStatic(nodekeys::TrackMode, trackmodes::Stabilize);
        c.node(planar)->params.setBase(nodekeys::PatternSize, 33);
        const QByteArray text
            = QJsonDocument(serialization::compositionToJson(c)).toJson(QJsonDocument::Compact);
        const Composition back
            = serialization::compositionFromJson(QJsonDocument::fromJson(text).object());
        QCOMPARE(back, c);
        const CompNode* n = back.node(planar);
        QVERIFY(n);
        QCOMPARE(n->type, nodetypes::PlanarTracker);
        QCOMPARE(n->params.values.value(nodekeys::planarCorner(2, true)).keys.size(), 18);
        QCOMPARE(n->params.value(nodekeys::planarCorner(1, false), 11, 0),
            c.node(planar)->params.value(nodekeys::planarCorner(1, false), 11, 0));
        QCOMPARE(n->params.value(nodekeys::RefFrame, 0, -1), 3.0);
        QCOMPARE(n->params.staticValue(nodekeys::TrackMode).toString(), trackmodes::Stabilize);
        ClipData clip;
        clip.id = 9;
        clip.kind = ClipKind::Solid;
        clip.duration = 50;
        clip.composition = c;
        QCOMPARE(serialization::clipFromJson(serialization::clipToJson(clip)), clip);
        // A planar node saved without parameters (older / hand-made file) loads with defaults.
        Composition bare = co::defaultComposition();
        const quint64 id = co::addNode(bare, nodetypes::PlanarTracker, {});
        bare.node(id)->params = {};
        const Composition loaded = serialization::compositionFromJson(QJsonDocument::fromJson(
            QJsonDocument(serialization::compositionToJson(bare)).toJson(QJsonDocument::Compact))
                                                                          .object());
        QCOMPARE(loaded, bare);
        QVERIFY(close(compmath::planarCorners(loaded.node(id)->params, 0, QSizeF(kW, kH))[0],
            QPointF(kW * 0.25, kH * 0.25), 1e-9));
    }

    void runnerFollowsATurningScalingPlane()
    {
        // Noise that moves, turns by 6 degrees and grows 7 % over 12 frames; the region is
        // the middle of the frame. The expected corners come from the very Transform that moves
        // the noise.
        quint64 mover = 0, planar = 0;
        const Composition c = turningNoise(&mover, &planar);
        const TimelineData tl = compTimeline(c);
        const quint64 clipId = tl.videoTracks[0].clips[0].id;
        const std::array<QPointF, 4> region {
            QPointF(100, 50), QPointF(220, 54), QPointF(216, 128), QPointF(104, 124)};
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            TrackJob job;
            job.firstFrame = 0;
            job.lastFrame = 12;
            job.planar = true;
            for (size_t i = 0; i < 4; ++i) {
                // Timeline px of a corner -> normalised, through the analysis pixel centre
                // convention (timeline x = image x + 0.5): the runner reads u * w - 0.5.
                job.startCorners[2 * i] = region[i].x() / kW;
                job.startCorners[2 * i + 1] = 1.0 - region[i].y() / kH;
            }
            job.patternFraction = 25.0 / kW;
            job.searchFraction = 48.0 / kW;
            const auto shared = snap(tl);
            job.source = [comp, shared, clipId, mover](int64_t f) {
                return comp->renderCompositionNode(
                    shared, clipId, mover, f, {}, Quality::Full, OutputImage);
            };
            TrackerRunner runner;
            QSignalSpy done(&runner, &TrackerRunner::finished);
            QVERIFY(runner.start(job));
            QVERIFY(done.wait(120000));
            QVERIFY2(done.first().first().value<TrackOutcome>() == TrackOutcome::Finished,
                qPrintable(done.first().at(1).toString()));
            QCOMPARE(int(runner.samples().size()), 13);
            for (const TrackSample& s : runner.samples()) {
                QVERIFY(s.planar);
                const QTransform m
                    = compmath::transformMatrix(c.node(mover)->params, s.frame, QSizeF(kW, kH));
                for (size_t i = 0; i < 4; ++i) {
                    const QPointF want = m.map(region[i]);
                    const QPointF got(s.corners[2 * i] * kW, (1.0 - s.corners[2 * i + 1]) * kH);
                    QVERIFY2(close(got, want, 0.6),
                        qPrintable(QStringLiteral("frame %1 corner %2: %3,%4 expected %5,%6")
                                       .arg(s.frame)
                                       .arg(i)
                                       .arg(got.x())
                                       .arg(got.y())
                                       .arg(want.x())
                                       .arg(want.y())));
                }
            }
        }
    }

    // ---- Linking a node's position to a track ----

    void anotherNodeFollowsAPointTrack()
    {
        Composition c = co::defaultComposition();
        const quint64 t = co::addNode(c, nodetypes::Tracker, {});
        const quint64 xf = co::addNode(c, nodetypes::Transform, {});
        const quint64 rect = co::addNode(c, nodetypes::RectangleMask, {});
        QVERIFY(canFollowTrack(*c.node(xf)) && canFollowTrack(*c.node(rect)));
        QVERIFY(!canFollowTrack(*c.node(t))); // a tracker's centre IS the track
        QVERIFY(!canFollowTrack(*c.node(mediaIn(c))));
        QVERIFY(trackSources(c).empty());
        QVERIFY(!linkToTrack(*c.node(xf), *c.node(t), 0)); // no track yet
        std::vector<TrackSample> samples;
        for (int f = 0; f <= 10; ++f)
            samples.push_back({f, 0.4 + 0.02 * f, 0.6 - 0.01 * f, 1.0});
        QVERIFY(storeTrack(*c.node(t), samples, 0));
        QCOMPARE(trackSources(c), std::vector<quint64> {t});
        QCOMPARE(trackFrames(*c.node(t)).size(), size_t(11));
        // The target sits at (0.5, 0.5) now; at frame 4 it must stay there and then move like
        // the track does: +0.02 per frame in x, -0.01 in y.
        QVERIFY(linkToTrack(*c.node(xf), *c.node(t), 4));
        const ParamSet& p = c.node(xf)->params;
        QCOMPARE(p.values.value(nodekeys::CenterX).keys.size(), 11);
        QVERIFY(std::abs(p.value(nodekeys::CenterX, 4, 0) - 0.5) < 1e-9);
        QVERIFY(std::abs(p.value(nodekeys::CenterY, 4, 0) - 0.5) < 1e-9);
        QVERIFY(std::abs(p.value(nodekeys::CenterX, 9, 0) - (0.5 + 0.02 * 5)) < 1e-9);
        QVERIFY(std::abs(p.value(nodekeys::CenterY, 9, 0) - (0.5 - 0.01 * 5)) < 1e-9);
        // The target's old keys are replaced, not mixed in.
        c.node(rect)->params.animated(nodekeys::CenterX, 0.5).setKey(40, 0.9);
        QVERIFY(linkToTrack(*c.node(rect), *c.node(t), 0));
        QCOMPARE(c.node(rect)->params.values.value(nodekeys::CenterX).keys.size(), 11);
        // Locked nodes are left alone.
        c.node(xf)->locked = true;
        QVERIFY(!linkToTrack(*c.node(xf), *c.node(t), 0));
    }

    void aNodeFollowsThePlanarTrackCentre()
    {
        Composition c = co::defaultComposition();
        const quint64 pt = co::addNode(c, nodetypes::PlanarTracker, {});
        const quint64 text = co::addNode(c, nodetypes::Text, {});
        QVERIFY(canFollowTrack(*c.node(text)));
        // A square that moves 100 px right (perspective-free), so the centre moves 100 px.
        const std::array<QPointF, 4> a {
            QPointF(60, 40), QPointF(160, 40), QPointF(160, 120), QPointF(60, 120)};
        std::array<QPointF, 4> b = a;
        for (QPointF& p : b)
            p += QPointF(100, 0);
        QVERIFY(storeTrack(*c.node(pt), {sampleOf(0, a), sampleOf(8, b)}, 0));
        QVERIFY(close(trackCentre(*c.node(pt), 0), QPointF(110.0 / kW, 1.0 - 80.0 / kH), 1e-9));
        QVERIFY(close(trackCentre(*c.node(pt), 8), QPointF(210.0 / kW, 1.0 - 80.0 / kH), 1e-9));
        QCOMPARE(trackSources(c), std::vector<quint64> {pt});
        c.node(text)->params.animated(nodekeys::CenterX, 0.5).setValue(0, 0.3);
        c.node(text)->params.animated(nodekeys::CenterY, 0.5).setValue(0, 0.7);
        QVERIFY(linkToTrack(*c.node(text), *c.node(pt), 0));
        QVERIFY(std::abs(c.node(text)->params.value(nodekeys::CenterX, 0, 0) - 0.3) < 1e-9);
        QVERIFY(std::abs(c.node(text)->params.value(nodekeys::CenterX, 8, 0) - (0.3 + 100.0 / kW))
            < 1e-9);
        QVERIFY(std::abs(c.node(text)->params.value(nodekeys::CenterY, 8, 0) - 0.7) < 1e-9);
    }
};

QTEST_MAIN(TestPlanarTrackerNode)
#include "test_planartrackernode.moc"
