// SPDX-License-Identifier: GPL-3.0-or-later
// Render correctness of nested content: a compound clip renders (picture and sound) exactly
// like the clips it was made from, frame-accurately through trims and nesting levels, and the
// instance's own transform / opacity apply to the nested picture as a whole. GPU and CPU.
#include "RenderTestUtil.h"
#include "core/CompoundOps.h"
#include "core/EditOps.h"
#include "render/AudioMixer.h"
#include "render/Compositor.h"

using namespace kk;
using namespace kk::test;

namespace {

/// @p parent's snapshot with @p nested in its nested-timeline map (what Timeline::snapshot()
/// produces inside a project).
std::shared_ptr<const TimelineData> withNested(
    const TimelineData& parent, const QVector<const TimelineData*>& nested)
{
    auto map = std::make_shared<NestedTimelineMap>();
    for (const TimelineData* n : nested)
        map->insert(n->uid, std::make_shared<const TimelineData>(*n));
    auto copy = std::make_shared<TimelineData>(parent);
    copy->nested.map = std::move(map);
    return copy;
}

QVector<quint64> allClips(const TimelineData& d)
{
    QVector<quint64> ids;
    for (TrackType type : {TrackType::Video, TrackType::Audio})
        for (const TrackData& t : d.tracks(type))
            for (const ClipData& c : t.clips)
                ids.append(c.id);
    return ids;
}

/// Largest per-channel difference between two images of the same size.
int maxDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
    }
    return worst;
}

} // namespace

class TestNested : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    QString m_fixtureError;

    void backendRows()
    {
        QTest::addColumn<bool>("useGpu");
        QTest::newRow("gpu") << true;
        QTest::newRow("cpu") << false;
    }
    Compositor* backend()
    {
        QFETCH(bool, useGpu);
        if (useGpu && !m_gpu->isGpu())
            return nullptr;
        return useGpu ? m_gpu.get() : m_cpu.get();
    }

    /// A layered scene: red solid on V1 [0, 50), a half-transparent title-like box (blue
    /// solid, zoomed and moved, 60 % opacity, keyframed position) on V2 [10, 40), a green
    /// solid fading in (transition) on V1 [50, 80).
    static TimelineData scene()
    {
        TimelineData tl = emptyTimeline(320, 180);
        tl.uid = 1;
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(200, 40, 30), 0, 50));
        ClipData green = solidClip(tl, QColor(30, 200, 60), 50, 30);
        green.transitionIn = TransitionData {};
        green.transitionIn->duration = 10;
        addClip(tl, TrackType::Video, 0, green);
        ClipData box = solidClip(tl, QColor(20, 60, 220), 10, 30);
        box.transform.setBase(keys::ZoomX, 0.4);
        box.transform.setBase(keys::ZoomY, 0.4);
        box.transform.values[keys::PosX].setKey(0, -60.0);
        box.transform.values[keys::PosX].setKey(29, 60.0);
        box.transform.setBase(keys::Opacity, 60.0);
        addClip(tl, TrackType::Video, 1, box);
        return tl;
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        if (!ensureFixtures(&m_fixtureError))
            qWarning("%s", qPrintable(m_fixtureError));
    }

    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    void compoundMatchesFlattened_data() { backendRows(); }
    void compoundMatchesFlattened()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        const TimelineData flat = scene();
        TimelineData parent = flat;
        const auto r
            = compound::collapse(parent, allClips(parent), 9, QStringLiteral("cc"), nullptr);
        QVERIFY(r);
        const auto nestedShot = withNested(parent, {&r->nested});
        const auto flatShot = snap(flat);
        for (const int64_t f : {0, 12, 25, 39, 45, 52, 58, 79}) {
            const QImage a = c->renderImage(flatShot, f);
            const QImage b = c->renderImage(nestedShot, f);
            QVERIFY2(maxDifference(a, b) <= 2,
                qPrintable(
                    QStringLiteral("frame %1 differs by %2").arg(f).arg(maxDifference(a, b))));
        }
    }

    void instanceTransformAppliesToWhole_data() { backendRows(); }
    void instanceTransformAppliesToWhole()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        // Two opaque solids stacked: at 50 % instance opacity the result is the TOP one at
        // 50 % (the nested picture fades as a whole), not a blend of both layers.
        TimelineData parent = emptyTimeline(64, 36);
        parent.uid = 1;
        addClip(parent, TrackType::Video, 0, solidClip(parent, QColor(255, 0, 0), 0, 10));
        addClip(parent, TrackType::Video, 1, solidClip(parent, QColor(0, 0, 255), 0, 10));
        const auto r
            = compound::collapse(parent, allClips(parent), 5, QStringLiteral("cc"), nullptr);
        QVERIFY(r);
        ClipData* inst = editops::clipById(parent, r->instanceIds[0]);
        inst->transform.setBase(keys::Opacity, 50.0);
        const QImage img = c->renderImage(withNested(parent, {&r->nested}), 3);
        const Px p = px(img, 32, 18);
        QVERIFY2(near(p, 0, 0, 128, 128, 3), describe(p).constData());

        // Zoom 0.5: the whole nested frame shrinks around the centre.
        inst->transform.setBase(keys::Opacity, 100.0);
        inst->transform.setBase(keys::ZoomX, 0.5);
        inst->transform.setBase(keys::ZoomY, 0.5);
        const QImage zoomed = c->renderImage(withNested(parent, {&r->nested}), 3);
        QVERIFY(near(px(zoomed, 32, 18), 0, 0, 255, 255, 3));
        QVERIFY(near(px(zoomed, 2, 2), 0, 0, 0, 0, 3)); // outside: transparent
    }

    void mediaThroughTwoLevels_data() { backendRows(); }
    void mediaThroughTwoLevels()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        // Level 2: the counter clip from source frame 25 at frame 0.
        TimelineData inner = emptyTimeline(1280, 720);
        inner.uid = 3;
        addClip(inner, TrackType::Video, 0,
            mediaClip(inner, QStringLiteral("v"), 0, 100, 1'000'000, 10'000'000));
        // Level 1: inner from its frame 10, placed at 5.
        TimelineData middle = emptyTimeline(1280, 720);
        middle.uid = 2;
        ClipData a = compound::makeNestedClip(inner, {});
        a.id = middle.allocateId();
        a.start = 5;
        a.duration = 50;
        a.sourceInUs = framesToUs(10, inner.fps);
        addClip(middle, TrackType::Video, 0, a);
        // Root: middle trimmed by 3 frames at 100.
        TimelineData root = emptyTimeline(1280, 720);
        root.uid = 1;
        ClipData b = compound::makeNestedClip(middle, {});
        b.id = root.allocateId();
        b.start = 100;
        b.duration = 40;
        b.sourceInUs = framesToUs(8, middle.fps);
        addClip(root, TrackType::Video, 0, b);
        auto media = std::make_shared<MediaPathMap>();
        media->insert(QStringLiteral("v"), fixture("h264_25fps.mp4"));
        c->setMediaPaths(media);
        const auto shot = withNested(root, {&middle, &inner});
        // root f -> middle 8 + (f - 100) -> inner 10 + (m - 5) -> source 25 + inner.
        for (const int64_t f : {100, 101, 102, 120, 110}) {
            const int64_t m = 8 + (f - 100);
            const int64_t i = 10 + (m - 5);
            QCOMPARE(readFrameIndex(c->renderImage(shot, f)), int(25 + i));
        }
        c->setMediaPaths({});
    }

    void missingOrCyclicNestedDrawsNothing()
    {
        TimelineData root = emptyTimeline(64, 36);
        root.uid = 1;
        TimelineData self = root;
        ClipData loop = compound::makeNestedClip(self, {});
        loop.id = root.allocateId();
        loop.duration = 10;
        addClip(root, TrackType::Video, 0, loop);
        // The root inside its own map (damaged data): the depth limit stops the recursion.
        auto map = std::make_shared<NestedTimelineMap>();
        auto copy = std::make_shared<TimelineData>(root);
        map->insert(1, std::make_shared<const TimelineData>(root));
        copy->nested.map = map;
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            const QImage img = c->renderImage(copy, 3);
            QVERIFY(near(px(img, 10, 10), 0, 0, 0, 0, 0));
            QVERIFY(near(px(c->renderImage(snap(root), 3), 10, 10), 0, 0, 0, 0, 0)); // offline
        }
    }

    void audioMatchesFlattened()
    {
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        auto paths = std::make_shared<MediaPathMap>();
        paths->insert(QStringLiteral("ramp"), fixture("mono_ramp_48k.wav"));
        TimelineData flat = emptyTimeline();
        flat.uid = 1;
        ClipData x = mediaClip(flat, QStringLiteral("ramp"), 10, 20, 200'000, 5'000'000);
        x.audio.setBase(keys::VolumeDb, -6.0);
        addClip(flat, TrackType::Audio, 0, x);
        addClip(flat, TrackType::Audio, 1,
            mediaClip(flat, QStringLiteral("ramp"), 20, 30, 0, 5'000'000));
        TimelineData parent = flat;
        const auto r
            = compound::collapse(parent, allClips(parent), 4, QStringLiteral("cc"), nullptr);
        QVERIFY(r);
        QCOMPARE(r->instanceIds.size(), 1); // audio only
        const auto nestedShot = withNested(parent, {&r->nested});

        AudioMixer a, b;
        a.setMediaPaths(paths);
        b.setMediaPaths(paths);
        constexpr int kBlock = 1024;
        std::vector<float> fa(kBlock * 2), fb(kBlock * 2);
        double worst = 0.0;
        for (int64_t s = 0; s < framesToSamples48k(60, flat.fps); s += kBlock) {
            a.mix(flat, s, kBlock, fa.data());
            b.mix(*nestedShot, s, kBlock, fb.data());
            for (int i = 0; i < kBlock * 2; ++i)
                worst = std::max(worst, double(std::abs(fa[size_t(i)] - fb[size_t(i)])));
        }
        QVERIFY2(worst < 1e-5, qPrintable(QString::number(worst)));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestNested>(argc, argv);
}

#include "test_nested.moc"
