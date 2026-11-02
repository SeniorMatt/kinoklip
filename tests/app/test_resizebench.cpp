// SPDX-License-Identifier: GPL-3.0-or-later
// Live-resize benchmark of the main window (owner report: "weird flickers and stutters" while
// resizing on Plasma Wayland). Resizes the Edit page in 40 steps of 10 px, the way a window
// manager's interactive resize does, and measures per step:
//   - the time to process the resize and repaint (layout + every widget's paint),
//   - how often the playback engine was asked for a new output size, i.e. a full re-render of
//     the frame and new output textures (the viewers coalesce those while resizing).
// Prints a comparison of the old behaviour (KK_VIEWER_RESIZE_SETTLE_MS=0: a re-render per
// step) and the coalesced one, and checks that the latter re-renders at most once per resize
// gesture. Runs on any platform (offscreen in ctest; under weston with
// scripts/wayland_session.sh for Wayland numbers).
//
// Numbers (Arch container, i5-10300H, GPU not passed in: Mesa llvmpipe; 1200x800 -> 1600x1000):
//   offscreen (raster viewers):         20.7 ms median / 30 ms max per step, 13 -> 1 re-renders
//   weston, raster viewers (KK_NO_OPENGL): 18.6 / 22 ms per step,             13 -> 1
//   weston, OpenGL viewers (default):   196 / 250 ms per step (llvmpipe),     13 -> 11
// A QOpenGLWidget makes Qt composite the whole top-level window with OpenGL: every resize
// step re-creates the window-sized texture, re-uploads the backing store and re-creates both
// viewers' FBOs. With llvmpipe that is 10x the raster cost; on a GPU it is a few ms, but it is
// still work per configure event on top of the layout, and the reason a native child window
// for the viewers (QOpenGLWindow in a window container, a separate wl_subsurface; the main
// window stays wl_shm) is the next step if resizing still stutters on real hardware. When a
// step takes longer than the settle delay (llvmpipe), the coalescing cannot help.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QWidget>

#include <algorithm>
#include <memory>

using namespace kk;

namespace {

constexpr int kSteps = 40;
constexpr int kStepPx = 10;

struct Result
{
    double medianMs = 0;
    double maxMs = 0;
    int outputSizeChanges = 0;
};

Result runResize(int settleMs)
{
    qputenv("KK_VIEWER_RESIZE_SETTLE_MS", QByteArray::number(settleMs));
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    actions.loadDefaultKeymap();
    registerStandardActions(actions);
    PlaybackEngine::Options options;
    options.noAudio = true;
    PlaybackEngine engine(std::move(options));
    engine.setMediaPool(project.mediaPool());
    engine.setTimeline(project.timeline());
    // A 1920x1080 timeline with a solid clip, so the viewers' output size follows their size.
    TimelineData tl = makeDefaultTimeline(QStringLiteral("Bench"), {25, 1}, 1920, 1080);
    ClipData clip;
    clip.id = tl.allocateId();
    clip.kind = ClipKind::Solid;
    clip.solidColor = Qt::darkCyan;
    clip.duration = 250;
    tl.videoTracks[0].clips.push_back(clip);
    project.timeline()->setData(tl);

    AppContext ctx;
    ctx.project = &project;
    ctx.selection = &selection;
    ctx.actions = &actions;
    ctx.playback = &engine;
    MainWindowOptions windowOptions;
    windowOptions.autosave = false;
    MainWindow window(ctx, windowOptions);
    window.setCurrentPage(QStringLiteral("edit"));
    window.resize(1200, 800);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window))
        return {};
    QTest::qWait(500); // first layout, first frame

    QVector<double> times;
    Result r;
    QSize lastOutput = engine.outputSize();
    QElapsedTimer timer;
    for (int i = 1; i <= kSteps; ++i) {
        timer.start();
        window.resize(1200 + i * kStepPx, 800 + i * kStepPx / 2);
        window.repaint(); // layout + paint of everything now (what the compositor waits for)
        QCoreApplication::processEvents();
        times.append(timer.nsecsElapsed() / 1e6);
        if (engine.outputSize() != lastOutput) {
            ++r.outputSizeChanges;
            lastOutput = engine.outputSize();
        }
        QTest::qWait(16); // one frame at 60 Hz between configure events
    }
    QTest::qWait(400); // the settle delay passes: the final size is rendered once
    if (engine.outputSize() != lastOutput)
        ++r.outputSizeChanges;
    std::sort(times.begin(), times.end());
    r.medianMs = times[times.size() / 2];
    r.maxMs = times.back();
    return r;
}

} // namespace

class TestResizeBench : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_resizebench"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void resizeCoalescesViewerRenders()
    {
        const Result before = runResize(0);
        const Result after = runResize(120);
        qInfo().noquote() << QStringLiteral(
            "resize %1 steps on %2: per-step median %3 ms / max %4 ms, %5 engine re-renders "
            "(re-render per step) -> median %6 ms / max %7 ms, %8 re-renders (coalesced)")
                                 .arg(kSteps)
                                 .arg(QGuiApplication::platformName())
                                 .arg(before.medianMs, 0, 'f', 1)
                                 .arg(before.maxMs, 0, 'f', 1)
                                 .arg(before.outputSizeChanges)
                                 .arg(after.medianMs, 0, 'f', 1)
                                 .arg(after.maxMs, 0, 'f', 1)
                                 .arg(after.outputSizeChanges);
        QVERIFY(before.outputSizeChanges > 5); // the old behaviour, for comparison
        // Steps slower than the settle delay (software GL) re-render between steps anyway.
        if (after.medianMs < 60)
            QVERIFY2(after.outputSizeChanges <= 2, "the viewers re-render while resizing");
    }

    /// Where a full repaint goes: each visible panel repainted alone (best of 3), printed.
    void panelPaintCosts()
    {
        Project project;
        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions windowOptions;
        windowOptions.autosave = false;
        MainWindow window(ctx, windowOptions);
        window.setCurrentPage(QStringLiteral("edit"));
        window.resize(1400, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(300);
        QElapsedTimer timer;
        auto best = [&](QWidget* w) {
            double ms = 1e9;
            for (int i = 0; i < 3; ++i) {
                timer.start();
                w->repaint();
                ms = std::min(ms, timer.nsecsElapsed() / 1e6);
            }
            return ms;
        };
        qInfo().noquote()
            << QStringLiteral("full window repaint: %1 ms").arg(best(&window), 0, 'f', 1);
        for (QWidget* w : window.findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            if (w->isVisible())
                qInfo().noquote() << QStringLiteral("  %1 %2x%3: %4 ms")
                                         .arg(QString::fromLatin1(w->metaObject()->className()))
                                         .arg(w->width())
                                         .arg(w->height())
                                         .arg(best(w), 0, 'f', 1);
        }
        for (QWidget* w : window.findChildren<QWidget*>()) {
            const QString name = QString::fromLatin1(w->metaObject()->className());
            if (w->isVisible() && name.startsWith(QLatin1String("kk::")) && w->width() > 150
                && w->height() > 100)
                qInfo().noquote() << QStringLiteral("    %1 %2x%3: %4 ms")
                                         .arg(name)
                                         .arg(w->width())
                                         .arg(w->height())
                                         .arg(best(w), 0, 'f', 2);
        }
    }
};

QTEST_MAIN(TestResizeBench)
#include "test_resizebench.moc"
