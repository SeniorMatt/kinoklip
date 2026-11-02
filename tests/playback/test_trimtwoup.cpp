// SPDX-License-Identifier: GPL-3.0-or-later
// Two-up trim view pictures (spec 8.15, 8.19): Program mode renders each side with its clip's
// look (effects, transform) through the program compositor; Source mode shows the source
// frames. Also the benchmark that decides the default: a roll dragged one frame per 16 ms over
// a 1080p clip pair, measuring the GUI-thread time per step and how far behind the pictures
// lag, in both modes (numbers in docs/polish-v3.md, Round 3).
#include "../media/MediaTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/viewer/TrimTwoUpView.h"
#include "ui/viewer/ViewerPanel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTest>

#include <algorithm>
#include <memory>

using namespace kk;

namespace {

/// Two clips of @p path back to back on V1 (cut at frame 100); the left one blurred, the
/// right one zoomed, so the program look differs from the source frames.
bool buildPair(Project& project, const QString& path)
{
    const ImportResult imported = project.importMedia({path});
    if (imported.importedIds.size() != 1)
        return false;
    const QString id = imported.importedIds[0];
    const media::MediaInfo& info = project.mediaPool()->item(id)->info;
    const Rational fps = info.frameRate.isValid() ? info.frameRate : Rational {25, 1};
    project.beginEdit(QStringLiteral("pair"));
    TimelineData& tl = project.timeline()->mutableData();
    tl = makeDefaultTimeline(QStringLiteral("pair"), fps, info.displayWidth, info.displayHeight);
    for (int i = 0; i < 2; ++i) {
        ClipData c;
        c.id = tl.allocateId();
        c.mediaId = id;
        c.name = i == 0 ? QStringLiteral("left") : QStringLiteral("right");
        c.sourceDurationUs = info.durationUs;
        c.start = 100 * i;
        c.duration = 100;
        c.sourceInUs = framesToUs(50 + 120 * i, fps);
        if (i == 0) {
            c.effects.push_back(EffectRegistry::instance().createEffect(
                QStringLiteral("gaussian_blur"), tl.allocateId()));
        } else {
            c.transform.setBase(keys::ZoomX, 1.5);
            c.transform.setBase(keys::ZoomY, 1.5);
        }
        tl.videoTracks[0].clips.push_back(c);
    }
    project.endEdit();
    return true;
}

struct Rig
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    std::unique_ptr<PlaybackEngine> engine;
    AppContext ctx;
    std::unique_ptr<ViewerPanel> viewer;

    bool build(QString* why)
    {
        if (!test::ensureFixtures(why) || !buildPair(project, test::fixture("perf_1080p.mp4"))) {
            if (why->isEmpty())
                *why = QStringLiteral("perf_1080p.mp4 missing");
            return false;
        }
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        engine = std::make_unique<PlaybackEngine>(std::move(options));
        engine->setMediaPool(project.mediaPool());
        engine->setTimeline(project.timeline());
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = engine.get();
        viewer = std::make_unique<ViewerPanel>(ViewerPanel::Mode::Timeline, ctx);
        viewer->resize(960, 600);
        viewer->show();
        return QTest::qWaitForWindowExposed(viewer.get());
    }
    [[nodiscard]] TrimTwoUpView* twoUp() const { return viewer->trimView(); }
    [[nodiscard]] const ClipData& clip(int i) const
    {
        return project.timeline()->data().videoTracks[0].clips[size_t(i)];
    }

    /// Rolls the cut to @p cut (live model change like a drag) and shows the two-up.
    void rollTo(int64_t cut)
    {
        TimelineData& d = project.timeline()->mutableData();
        ClipData& a = d.videoTracks[0].clips[0];
        ClipData& b = d.videoTracks[0].clips[1];
        const int64_t delta = cut - b.start;
        a.duration += delta;
        b.sourceInUs += framesToUs(delta, d.fps);
        b.start = cut;
        b.duration -= delta;
        project.timeline()->notifyChanged();
        TrimPreview p;
        p.left = {a.id, a.mediaId, a.sourceTimeUsAt(cut - 1, d.fps), cut - 1, a.name};
        p.right = {b.id, b.mediaId, b.sourceInUs, cut, b.name};
        twoUp()->setPreview(p, d.fps);
    }
};

/// Result of one simulated roll drag.
struct DragStats
{
    double guiMsPerStep = 0; ///< GUI thread time of a step (model change + preview update)
    double upToDateShare = 0; ///< steps after which both sides showed the wanted frame in 16 ms
    double settleMs = 0; ///< after the last step: until both sides show the final frames
    bool settled = false; ///< both sides caught up within 5 s
};

DragStats dragRoll(Rig& rig, TrimTwoUpView::Pictures mode)
{
    constexpr int kSteps = 40;
    constexpr int kStepMs = 16; ///< one mouse move per display frame
    rig.twoUp()->setPictures(mode);
    rig.project.beginEdit(QStringLiteral("Roll"));
    DragStats stats;
    QElapsedTimer gui;
    qint64 guiNs = 0;
    int upToDate = 0;
    for (int i = 1; i <= kSteps; ++i) {
        gui.start();
        rig.rollTo(100 + i);
        guiNs += gui.nsecsElapsed();
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < kStepMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        }
        if (rig.twoUp()->hasPicture(0) && rig.twoUp()->hasPicture(1))
            ++upToDate;
    }
    QElapsedTimer settle;
    settle.start();
    stats.settled = QTest::qWaitFor(
        [&] { return rig.twoUp()->hasPicture(0) && rig.twoUp()->hasPicture(1); }, 5000);
    stats.settleMs = settle.nsecsElapsed() / 1e6;
    rig.project.cancelEdit();
    rig.project.endEdit();
    rig.twoUp()->setPreview({}, rig.project.timeline()->data().fps);
    stats.guiMsPerStep = guiNs / 1e6 / kSteps;
    stats.upToDateShare = double(upToDate) / kSteps;
    return stats;
}

} // namespace

class TestTrimTwoUp : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initResources(); }

    // Program pictures carry the clip's look: the zoomed right side differs from its source
    // frame; Source mode is the fallback without a GPU compositor.
    void programPicturesShowTheLook()
    {
        Rig rig;
        QString why;
        if (!rig.build(&why))
            QSKIP(qPrintable(why));
        TrimTwoUpView* view = rig.twoUp();
        if (!rig.engine->compositor()->isGpu()) {
            QCOMPARE(view->pictures(), TrimTwoUpView::Pictures::Source);
            QSKIP("no GPU compositor: the two-up uses source frames");
        }
        rig.project.beginEdit(QStringLiteral("Roll"));
        view->setPictures(TrimTwoUpView::Pictures::Source);
        rig.rollTo(101);
        QVERIFY(QTest::qWaitFor([&] { return view->hasPicture(1); }, 5000));
        const QImage source = view->grab().toImage();
        view->setPictures(TrimTwoUpView::Pictures::Program);
        QCOMPARE(view->pictures(), TrimTwoUpView::Pictures::Program);
        rig.rollTo(102);
        rig.rollTo(101);
        QVERIFY(QTest::qWaitFor([&] { return view->hasPicture(0) && view->hasPicture(1); }, 5000));
        const QImage program = view->grab().toImage();
        QVERIFY(program != source);
        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty()) {
            source.save(dir + QStringLiteral("/twoup-source.png"));
            program.save(dir + QStringLiteral("/twoup-program.png"));
        }
        rig.project.cancelEdit();
        rig.project.endEdit();
    }

    // The benchmark behind the default mode (both modes, same drag).
    void rollDragCost()
    {
        Rig rig;
        QString why;
        if (!rig.build(&why))
            QSKIP(qPrintable(why));
        if (!rig.engine->compositor()->isGpu())
            QSKIP("no GPU compositor");
        dragRoll(rig, TrimTwoUpView::Pictures::Source); // warm up decoders and caches
        const DragStats source = dragRoll(rig, TrimTwoUpView::Pictures::Source);
        const DragStats program = dragRoll(rig, TrimTwoUpView::Pictures::Program);
        qInfo("two-up roll drag, 40 steps at 16 ms, 1080p h264:");
        qInfo("  source : GUI %.2f ms/step, up to date after %.0f %% of steps, settle %.0f ms",
            source.guiMsPerStep, source.upToDateShare * 100, source.settleMs);
        qInfo("  program: GUI %.2f ms/step, up to date after %.0f %% of steps, settle %.0f ms",
            program.guiMsPerStep, program.upToDateShare * 100, program.settleMs);
        QVERIFY(source.settled && program.settled);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    TestTrimTwoUp test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_trimtwoup.moc"
