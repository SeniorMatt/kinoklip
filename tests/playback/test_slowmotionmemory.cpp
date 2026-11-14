// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #8: a clip at 12.5 % played while the viewer changes size (full-screen toggles) must not
// grow memory: the compositor's output frames (GPU textures) stay bounded, nothing seeks while
// playing, and the decoders do not pile up. Prints the numbers per speed (25 % for comparison).
#include "../media/MediaTestUtil.h"
#include "PlaybackTestUtil.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/RetimeOps.h"
#include "media/MediaInfo.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "render/GlOutput.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/StandardActions.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerTransport.h"
#include "core/SelectionModel.h"

#include <QApplication>
#include <QFile>
#include <QHBoxLayout>
#include <QTest>

using namespace kk;

namespace {

/// Resident set size in MiB.
double rssMiB()
{
    QFile f(QStringLiteral("/proc/self/statm"));
    if (!f.open(QIODevice::ReadOnly))
        return 0.0;
    const QList<QByteArray> parts = f.readAll().split(' ');
    return parts.size() > 1 ? parts[1].toDouble() * 4096.0 / (1 << 20) : 0.0;
}

void buildClip(Project& project, const QString& path, double speed)
{
    const ImportResult imported = project.importMedia({path});
    QCOMPARE(imported.importedIds.size(), 1);
    const media::MediaInfo& info = project.mediaPool()->item(imported.importedIds[0])->info;
    const Rational fps = info.frameRate.isValid() ? info.frameRate : Rational {25, 1};
    project.beginEdit(QStringLiteral("slow"));
    TimelineData& tl = project.timeline()->mutableData();
    tl = makeDefaultTimeline(QStringLiteral("slow"), fps, 1280, 720, 2, 2);
    ClipData c;
    c.id = tl.allocateId();
    c.mediaId = imported.importedIds[0];
    c.sourceDurationUs = info.durationUs;
    c.duration = std::max<int64_t>(1, usToFrames(info.durationUs, fps)) / 2;
    c.linkGroup = 1;
    tl.videoTracks[0].clips.push_back(c);
    if (info.hasAudio) {
        ClipData a = c;
        a.id = tl.allocateId();
        tl.audioTracks[0].clips.push_back(a);
    }
    const quint64 id = c.id;
    project.endEdit();
    project.beginEdit(QStringLiteral("speed"));
    retimeops::SpeedChange change;
    change.speed = speed;
    retimeops::changeClipSpeed(project.timeline()->mutableData(), id, change);
    project.endEdit();
}

} // namespace

class TestSlowMotionMemory : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void resizesWhilePlaying_data()
    {
        QTest::addColumn<double>("speed");
        QTest::newRow("25 percent") << 0.25;
        QTest::newRow("12.5 percent") << 0.125;
    }

    void resizesWhilePlaying()
    {
        QFETCH(double, speed);
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Project project;
        buildClip(project, test::fixture("perf_1080p.mp4"), speed);

        PlaybackEngine::Options options;
        options.audio = std::make_unique<test::FakeAudioOutput>(10, 40);
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        const bool gpu = engine.compositor()->isGpu();
        int viewer = 0;
        int fullscreen = 0;
        engine.setOutputClient(&viewer, QSize(640, 360), gpu);
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 10000));
        engine.play();
        const double rssBefore = rssMiB();
        int maxSlots = 0;
        int maxInUse = 0;
        int maxDecoders = 0;
        int64_t maxBytes = 0;
        // Six toggles: the second viewer (1280x720 here, the whole picture) opens and closes.
        for (int i = 0; i < 6; ++i) {
            if (i % 2 == 0)
                engine.setOutputClient(&fullscreen, QSize(1280, 720), gpu);
            else
                engine.removeOutputClient(&fullscreen);
            for (int t = 0; t < 8; ++t) {
                QTest::qWait(250);
                const render::ResourceStats s = engine.compositor()->resourceStats();
                maxSlots = std::max(maxSlots, s.outputSlots);
                maxInUse = std::max(maxInUse, s.outputSlotsInUse);
                maxDecoders = std::max(maxDecoders, s.decoders);
                maxBytes = std::max(maxBytes, s.outputSlotBytes);
            }
        }
        const PlaybackEngine::Stats stats = engine.stats();
        const double rssAfter = rssMiB();
        engine.pause();
        qInfo("speed %.3f (%s): output frames max %d (in use max %d, %.0f MiB), decoders max %d, "
              "seeks while playing %lld, frames shown %lld, RSS %.0f -> %.0f MiB",
            speed, gpu ? "gpu" : "cpu", maxSlots, maxInUse, double(maxBytes) / (1 << 20),
            maxDecoders, qlonglong(stats.seeksWhilePlaying), qlonglong(stats.presented),
            rssBefore, rssAfter);
        QCOMPARE(stats.seeksWhilePlaying, 0);
        QVERIFY(stats.presented > 20);
        if (gpu) {
            QVERIFY2(maxInUse <= 12, "a viewer holds a handful of frames, not dozens");
            QVERIFY(maxSlots <= render::GlOutput::kMaxSlots);
        }
        QVERIFY(maxDecoders <= 4);
        QVERIFY2(rssAfter - rssBefore < 400, "memory grows while playing in slow motion");
    }

    /// The same through the real full-screen viewer (Ctrl+F): a second ViewerPanel with its own
    /// GL surface opens and closes over the playing program viewer.
    void fullScreenToggles()
    {
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Project project;
        buildClip(project, test::fixture("perf_1080p.mp4"), 0.125);
        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        PlaybackEngine::Options options;
        options.audio = std::make_unique<test::FakeAudioOutput>(10, 40);
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = &engine;

        QWidget host;
        auto* layout = new QHBoxLayout(&host);
        auto* program = new ViewerPanel(ViewerPanel::Mode::Timeline, ctx);
        layout->addWidget(program);
        host.resize(960, 540);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        ViewerTransport transport(ctx);
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 10000));
        engine.play();
        const double rssBefore = rssMiB();
        int maxSlots = 0;
        int maxDecoders = 0;
        for (int i = 0; i < 6; ++i) {
            transport.setFullScreenViewer(true);
            QTest::qWait(800);
            QVERIFY(transport.isFullScreenViewerOpen());
            transport.setFullScreenViewer(false);
            QTRY_VERIFY(!transport.isFullScreenViewerOpen());
            QTest::qWait(800);
            const render::ResourceStats s = engine.compositor()->resourceStats();
            maxSlots = std::max(maxSlots, s.outputSlots);
            maxDecoders = std::max(maxDecoders, s.decoders);
        }
        const PlaybackEngine::Stats stats = engine.stats();
        const double rssAfter = rssMiB();
        engine.pause();
        qInfo("full-screen toggles: output frames max %d, decoders max %d, seeks while playing "
              "%lld, frames shown %lld, RSS %.0f -> %.0f MiB",
            maxSlots, maxDecoders, qlonglong(stats.seeksWhilePlaying), qlonglong(stats.presented),
            rssBefore, rssAfter);
        QCOMPARE(stats.seeksWhilePlaying, 0);
        QVERIFY(maxSlots <= render::GlOutput::kMaxSlots);
        QVERIFY(maxDecoders <= 4);
        QVERIFY2(rssAfter - rssBefore < 400, "memory grows with full-screen toggles");
    }
};

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts); // as main() does
    QApplication app(argc, argv);
    TestSlowMotionMemory test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_slowmotionmemory.moc"
