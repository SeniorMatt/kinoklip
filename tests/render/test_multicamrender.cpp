// SPDX-License-Identifier: GPL-3.0-or-later
// Render correctness of multicam clips: a multicam clip shows its active angle only (picture
// and sound), cuts switch the picture exactly at the cut frame, and the viewer's angle grid
// shows every angle in its cell. GPU and CPU.
#include "RenderTestUtil.h"
#include "core/EditOps.h"
#include "core/Multicam.h"
#include "render/AudioMixer.h"
#include "render/Compositor.h"

using namespace kk;
using namespace kk::test;
namespace mc = kk::multicam;

namespace {

/// What Project::nestedTimelinesFor() produces: the multicam timeline and its angle views.
std::shared_ptr<const TimelineData> withMulticam(const TimelineData& parent, const TimelineData& m)
{
    auto map = std::make_shared<NestedTimelineMap>();
    map->insert(m.uid, std::make_shared<const TimelineData>(m));
    for (int a = 0; a < mc::angleCount(m); ++a)
        map->insert(
            multicamViewKey(m.uid, a), std::make_shared<const TimelineData>(mc::angleView(m, a)));
    auto copy = std::make_shared<TimelineData>(parent);
    copy->nested.map = std::move(map);
    return copy;
}

const QColor kAngleColors[] = {QColor(220, 30, 30), QColor(30, 200, 40), QColor(30, 50, 230)};

/// A multicam timeline (uid 5) with three angles of solid colour (red, green, blue), 100
/// frames each; V track k = angle k.
TimelineData colourMulticam(int width = 64, int height = 36)
{
    TimelineData m = emptyTimeline(width, height);
    m.uid = 5;
    m.multicam = m.compound = true;
    m.videoTracks.resize(3);
    m.audioTracks.resize(3);
    for (int k = 0; k < 3; ++k) {
        m.videoTracks[size_t(k)].id = m.allocateId();
        m.videoTracks[size_t(k)].name = QStringLiteral("Cam %1").arg(k + 1);
        m.audioTracks[size_t(k)].id = m.allocateId();
        m.audioTracks[size_t(k)].type = TrackType::Audio;
        addClip(m, TrackType::Video, k, solidClip(m, kAngleColors[k], 0, 100));
    }
    return m;
}

bool isColour(const QImage& img, int x, int y, const QColor& c)
{
    return near(px(img, x, y), c.red(), c.green(), c.blue(), 255, 3);
}

} // namespace

class TestMulticamRender : public QObject
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

    void activeAngleAndCuts_data() { backendRows(); }
    void activeAngleAndCuts()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        const TimelineData m = colourMulticam();
        TimelineData parent = emptyTimeline(64, 36);
        parent.uid = 1;
        ClipData clip = mc::makeMulticamClip(m, {}, 1);
        clip.id = parent.allocateId();
        addClip(parent, TrackType::Video, 0, clip);
        // Angle 1 (green) only, although angle 2 (blue) is the top track of the multicam.
        QVERIFY(isColour(c->renderImage(withMulticam(parent, m), 10), 32, 18, kAngleColors[1]));
        // Without an angle the whole multicam timeline: the top track wins.
        TimelineData plain = parent;
        plain.videoTracks[0].clips[0].multicamAngle = -1;
        QVERIFY(isColour(c->renderImage(withMulticam(plain, m), 10), 32, 18, kAngleColors[2]));

        // Cut to angle 0 at frame 40, to angle 2 at 70: exact at the cut frames.
        const compound::TimelineLookup lk
            = [&m](quint64 uid) { return uid == m.uid ? &m : nullptr; };
        QVERIFY(!mc::cutToAngle(parent, 40, 0, mc::AngleTarget::Both, lk).isEmpty());
        QVERIFY(!mc::cutToAngle(parent, 70, 2, mc::AngleTarget::Video, lk).isEmpty());
        const auto shot = withMulticam(parent, m);
        QVERIFY(isColour(c->renderImage(shot, 39), 32, 18, kAngleColors[1]));
        QVERIFY(isColour(c->renderImage(shot, 40), 32, 18, kAngleColors[0]));
        QVERIFY(isColour(c->renderImage(shot, 69), 32, 18, kAngleColors[0]));
        QVERIFY(isColour(c->renderImage(shot, 70), 32, 18, kAngleColors[2]));
        // Switching the middle segment changes only it.
        QVERIFY(mc::switchAngle(
            parent, parent.videoTracks[0].clips[1].id, 2, mc::AngleTarget::Video, lk));
        const auto switched = withMulticam(parent, m);
        QVERIFY(isColour(c->renderImage(switched, 50), 32, 18, kAngleColors[2]));
        QVERIFY(isColour(c->renderImage(switched, 20), 32, 18, kAngleColors[1]));
    }

    void gridShowsEveryAngle_data() { backendRows(); }
    void gridShowsEveryAngle()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        const TimelineData m = colourMulticam(128, 72);
        const TimelineData grid = mc::gridTimeline(m, 2, 0, 0, {});
        const QImage img = c->renderImage(withMulticam(grid, m), 5);
        QCOMPARE(img.size(), QSize(128, 72));
        QVERIFY(isColour(img, 32, 18, kAngleColors[0])); // top left: angle 1
        QVERIFY(isColour(img, 96, 18, kAngleColors[1])); // top right: angle 2
        QVERIFY(isColour(img, 32, 54, kAngleColors[2])); // bottom left: angle 3
        QVERIFY(near(px(img, 96, 54), 0, 0, 0, 0, 3)); // bottom right: empty
    }

    void audioFollowsAudioAngle()
    {
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        auto paths = std::make_shared<MediaPathMap>();
        paths->insert(QStringLiteral("ramp"), fixture("mono_ramp_48k.wav"));
        // Angle 0: the ramp; angle 1: silence (the ramp at -96 dB); angle 2: nothing.
        TimelineData m = colourMulticam();
        addClip(m, TrackType::Audio, 0, mediaClip(m, QStringLiteral("ramp"), 0, 50, 0, 5'000'000));
        ClipData quiet = mediaClip(m, QStringLiteral("ramp"), 0, 50, 0, 5'000'000);
        quiet.audio.setBase(keys::VolumeDb, -96.0);
        addClip(m, TrackType::Audio, 1, quiet);
        TimelineData parent = emptyTimeline();
        parent.uid = 1;
        ClipData clip = mc::makeMulticamClip(m, {}, 1);
        clip.id = parent.allocateId();
        addClip(parent, TrackType::Audio, 0, clip);
        const compound::TimelineLookup lk
            = [&m](quint64 uid) { return uid == m.uid ? &m : nullptr; };
        QVERIFY(!mc::cutToAngle(parent, 25, 0, mc::AngleTarget::Audio, lk).isEmpty());
        const auto shot = withMulticam(parent, m);
        AudioMixer mixer;
        mixer.setMediaPaths(paths);
        auto peak = [&](int64_t fromFrame, int64_t toFrame) {
            constexpr int kBlock = 960;
            std::vector<float> buf(kBlock * 2);
            double worst = 0.0;
            for (int64_t s = framesToSamples48k(fromFrame, parent.fps);
                 s < framesToSamples48k(toFrame, parent.fps); s += kBlock) {
                mixer.mix(*shot, s, kBlock, buf.data());
                for (const float x : buf)
                    worst = std::max(worst, double(std::abs(x)));
            }
            return worst;
        };
        QVERIFY2(peak(2, 24) < 1e-3, "angle 1 is silent");
        QVERIFY2(peak(26, 48) > 0.05, "angle 0 after the audio cut");
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestMulticamRender>(argc, argv);
}

#include "test_multicamrender.moc"
