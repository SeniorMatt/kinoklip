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
// sweep: 1280x720 -> 1920x1080 in 8 px steps at 60 Hz per Edit page configuration (dual,
// single viewer, clip loaded, Keyframes docked), timed by ResizeProfiler (resize event to the
// last paint at that size); checks that hidden pages do no work and that each viewer is laid
// out once per step. interactive: the same window resized by a compositor
// (scripts/resize_sweep.sh, docs/resize-performance.md). KK_RESIZEBENCH_PROFILE=1 prints where
// the time goes per receiver class and event type, KK_RESIZEBENCH_TRACE_PAINTS=1 every paint.
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
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/LiveResize.h"
#include "ui/common/ResizeProfiler.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QExposeEvent>
#include <QFileInfo>
#include <QHash>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTest>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <functional>
#include <memory>

using namespace kk;

/// QApplication that, while profiling, adds up the time spent handling each (receiver class,
/// event type) pair, exclusive of nested events: where a resize step's time goes
/// (KK_RESIZEBENCH_PROFILE=1 prints the top entries after each sweep).
class ProfilingApplication : public QApplication
{
public:
    using QApplication::QApplication;
    bool profiling = false;
    QHash<QString, qint64> exclusiveNs;
    QHash<QString, int> eventCounts;

    bool notify(QObject* receiver, QEvent* event) override
    {
        if (!profiling)
            return QApplication::notify(receiver, event);
        // Before notify(): the receiver may be deleted by its event (DeferredDelete).
        const QString key = QString::fromLatin1(receiver->metaObject()->className())
            + QLatin1Char('/') + QString::number(int(event->type()));
        QElapsedTimer timer;
        timer.start();
        m_childNs.push_back(0);
        const bool result = QApplication::notify(receiver, event);
        const qint64 total = timer.nsecsElapsed();
        const qint64 children = m_childNs.back();
        m_childNs.pop_back();
        if (!m_childNs.isEmpty())
            m_childNs.back() += total;
        exclusiveNs[key] += total - children;
        ++eventCounts[key];
        return result;
    }

private:
    QVector<qint64> m_childNs;
};

ProfilingApplication* profilingApp()
{
    return static_cast<ProfilingApplication*>(qApp);
}

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
    TimelineData tl = makeDefaultTimeline(QStringLiteral("Bench"), {25, 1}, 1920, 1080, 2, 2);
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

/// Counts the resize events widgets receive while installed on the application.
class ResizeCounter : public QObject
{
public:
    QHash<QObject*, int> counts;
    int total = 0;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        // KK_RESIZEBENCH_TRACE_PAINTS=1: which widgets paint or change geometry,
        // to find repaints after a step. Compare with WAYLAND_DEBUG=client.
        static const bool tracePaints = qEnvironmentVariableIsSet("KK_RESIZEBENCH_TRACE_PAINTS");
        const QEvent::Type type = event->type();
        if (tracePaints && watched->isWidgetType()
            && (type == QEvent::Paint || type == QEvent::Move || type == QEvent::Resize
                || type == QEvent::Show || type == QEvent::Hide)) {
            auto* w = static_cast<QWidget*>(watched);
            qInfo("%s %s %s %dx%d", type == QEvent::Paint ? "paint" : "geom",
                w->metaObject()->className(), qPrintable(w->objectName()), w->width(), w->height());
        }
        if (event->type() == QEvent::Resize && watched->isWidgetType()) {
            ++counts[watched];
            ++total;
        }
        return false;
    }
};

/// Starts the window manager's interactive resize of @p window's bottom-right corner on a left
/// press inside it (while installed on the application).
class PressStartsResize : public QObject
{
public:
    explicit PressStartsResize(QWidget* window)
        : m_window(window)
    { }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::MouseButtonPress || !watched->isWidgetType()
            || static_cast<QWidget*>(watched)->window() != m_window)
            return false;
        if (QWindow* handle = m_window->windowHandle())
            qInfo("startSystemResize: %s",
                handle->startSystemResize(Qt::RightEdge | Qt::BottomEdge) ? "started" : "refused");
        return true;
    }

private:
    QWidget* m_window;
};

/// Edit page configurations of the sweep.
enum class Config {
    Dual,
    Single,
    Clip,
    Keyframes
};

/// Result of one interactive-style sweep.
struct Sweep
{
    ResizeProfiler::Summary frames; ///< resize event -> frame, per step
    double resizesPerStep = 0; ///< widget resize events per step (layout work)
    int maxSurfaceResizesPerStep = 0; ///< most resize events one viewer surface got in a step
    int hiddenPageResizes = 0; ///< resize events of widgets on hidden pages
    int outputSizeChanges = 0; ///< engine re-renders for a new viewer size during the sweep
};

constexpr QSize kSweepFrom(1280, 720);
constexpr QSize kSweepTo(1920, 1080);
constexpr int kSweepStepPx = 8; ///< width change per step (the height follows 16:9)
constexpr int kSweepFrameMs = 16; ///< one configure event per 60 Hz refresh

/// Resizes the main window from kSweepFrom to kSweepTo in kSweepStepPx steps, one per frame,
/// in configuration @p config, and measures every step with a ResizeProfiler. Media for
/// Config::Clip: @p clipPath (imported, appended to the timeline and shown in the source
/// viewer). With @p interactiveMs > 0 the window is not resized by the test: it waits that long
/// for a resize driven from outside (a compositor's interactive resize, see interactive()).
Sweep runSweep(Config config, const QString& clipPath, int interactiveMs = 0)
{
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
    AppContext ctx;
    ctx.project = &project;
    ctx.selection = &selection;
    ctx.actions = &actions;
    ctx.playback = &engine;
    MainWindowOptions windowOptions;
    windowOptions.autosave = false;
    windowOptions.restoreLayout = false;
    windowOptions.saveLayout = false;
    MainWindow window(ctx, windowOptions);
    window.setCurrentPage(QStringLiteral("edit"));
    EditPage* edit = window.editPage();
    if (config == Config::Single)
        edit->setSingleViewer(true);
    if (config == Config::Keyframes)
        edit->setPanelVisible(PanelId::Keyframes, true);
    window.resize(kSweepFrom);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window))
        return {};
    if (config == Config::Clip) {
        const ImportResult imported = project.importMedia({clipPath});
        if (imported.importedIds.size() == 1) {
            const QString id = imported.importedIds.front();
            edit->timeline()->controller()->editSources(
                {SourceSelection {id, 0, 0}}, SourceEditMode::AppendAtEnd);
            edit->sourceViewer()->setSourceMedia(id);
            project.timeline()->setPlayhead(10);
        }
        QTest::qWait(1500); // probe, first frames, thumbnails, waveform
    }
    QTest::qWait(500); // first layout, first frame

    // Widgets of the pages that are not shown: they must not do work on a resize.
    QSet<QObject*> hidden;
    if (auto* stack = window.findChild<QStackedWidget*>()) {
        for (int i = 0; i < stack->count(); ++i) {
            QWidget* page = stack->widget(i);
            if (page == stack->currentWidget())
                continue;
            hidden.insert(page);
            for (QWidget* w : page->findChildren<QWidget*>())
                hidden.insert(w);
        }
    }
    QSet<QObject*> surfaces;
    for (ViewerPanel* viewer : window.findChildren<ViewerPanel*>())
        surfaces.insert(viewer->surface()->widget());

    if (qEnvironmentVariableIsSet("KK_RESIZEBENCH_TRACE_PAINTS")) {
        for (QScrollBar* bar : edit->timeline()->findChildren<QScrollBar*>()) {
            QObject::connect(bar, &QScrollBar::rangeChanged, bar, [bar](int min, int max) {
                qInfo("scrollbar %s range %d..%d", bar->orientation() == Qt::Horizontal ? "h" : "v",
                    min, max);
            });
            QObject::connect(bar, &QScrollBar::valueChanged, bar, [bar](int v) {
                qInfo("scrollbar %s value %d", bar->orientation() == Qt::Horizontal ? "h" : "v", v);
            });
        }
    }
    ResizeProfiler* profiler = ResizeProfiler::attach(&window, true);
    ResizeCounter counter;
    qApp->installEventFilter(&counter);
    Sweep r;
    QSize lastOutput = engine.outputSize();
    const bool profile = qEnvironmentVariableIsSet("KK_RESIZEBENCH_PROFILE");
    profilingApp()->exclusiveNs.clear();
    profilingApp()->eventCounts.clear();
    profilingApp()->profiling = profile;
    const int steps
        = interactiveMs > 0 ? 0 : (kSweepTo.width() - kSweepFrom.width()) / kSweepStepPx;
    if (interactiveMs > 0) {
        // KK_RESIZEBENCH_PRESS_RESIZES=1: a left press anywhere in the window starts the window
        // manager's interactive resize of the bottom-right corner (QWindow::startSystemResize,
        // what a press on a client-side decoration edge does), so a plain pointer drag drives
        // it on any compositor.
        PressStartsResize pressResizes(&window);
        if (qEnvironmentVariableIntValue("KK_RESIZEBENCH_PRESS_RESIZES") > 0)
            qApp->installEventFilter(&pressResizes);
        qInfo("window shown at %dx%d; waiting %d ms for an interactive resize", window.width(),
            window.height(), interactiveMs);
        QTest::qWait(interactiveMs);
        qApp->removeEventFilter(&pressResizes);
    }
    for (int i = 1; i <= steps; ++i) {
        const int width = kSweepFrom.width() + i * kSweepStepPx;
        counter.counts.clear();
        window.resize(
            width, kSweepFrom.height() + (kSweepTo.height() - kSweepFrom.height()) * i / steps);
        QTest::qWait(kSweepFrameMs); // the frame at this size is painted and flushed
        for (auto it = counter.counts.cbegin(); it != counter.counts.cend(); ++it) {
            if (hidden.contains(it.key()))
                r.hiddenPageResizes += it.value();
            if (surfaces.contains(it.key()))
                r.maxSurfaceResizesPerStep = std::max(r.maxSurfaceResizesPerStep, it.value());
        }
        if (engine.outputSize() != lastOutput) {
            ++r.outputSizeChanges;
            lastOutput = engine.outputSize();
        }
    }
    qApp->removeEventFilter(&counter);
    profilingApp()->profiling = false;
    if (profile) {
        QVector<QPair<qint64, QString>> top;
        for (auto it = profilingApp()->exclusiveNs.cbegin();
             it != profilingApp()->exclusiveNs.cend(); ++it)
            top.append({it.value(), it.key()});
        std::sort(top.begin(), top.end(), std::greater<>());
        const qsizetype stepCount = std::max<qsizetype>(1, profiler->steps().size());
        for (qsizetype i = 0; i < std::min<qsizetype>(25, top.size()); ++i)
            qInfo("  %-50s %7.3f ms per step, %5.1f events per step", qPrintable(top[i].second),
                top[i].first / 1e6 / double(stepCount),
                profilingApp()->eventCounts.value(top[i].second) / double(stepCount));
    }
    r.frames = ResizeProfiler::summarize(profiler->steps());
    r.resizesPerStep = double(counter.total) / std::max<qsizetype>(1, profiler->steps().size());
    if (interactiveMs > 0) {
        r.hiddenPageResizes = 0;
        for (auto it = counter.counts.cbegin(); it != counter.counts.cend(); ++it)
            if (hidden.contains(it.key()))
                r.hiddenPageResizes += it.value();
    }
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
        // KK_RESIZEBENCH_STYLE=Fusion etc.: compare widget styles.
        if (qEnvironmentVariableIsSet("KK_RESIZEBENCH_STYLE"))
            theme::setStyle(qEnvironmentVariable("KK_RESIZEBENCH_STYLE"));
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

    /// Manual measurement under a real compositor (skipped unless KK_RESIZEBENCH_WAIT_MS is
    /// set): shows the Edit page at 1280x720 (KK_RESIZEBENCH_CONFIG = dual, single, clip or
    /// keyframes), waits while something else resizes the window interactively (kk_fakeinput
    /// under KWin, see docs/resize-performance.md), then prints the per-step statistics.
    void interactive()
    {
        const int waitMs = qEnvironmentVariableIntValue("KK_RESIZEBENCH_WAIT_MS");
        if (waitMs <= 0)
            QSKIP("set KK_RESIZEBENCH_WAIT_MS for a manual interactive measurement");
        const QString name = qEnvironmentVariable("KK_RESIZEBENCH_CONFIG", QStringLiteral("dual"));
        const Config config = name == QLatin1String("single") ? Config::Single
            : name == QLatin1String("clip")                   ? Config::Clip
            : name == QLatin1String("keyframes")              ? Config::Keyframes
                                                              : Config::Dual;
        const Sweep r = runSweep(config, test::fixture("h264_25fps.mp4"), waitMs);
        qInfo().noquote() << QStringLiteral(
            "interactive %1 on %2: %3 steps, frame median %4 ms / p90 %5 ms / max %6 ms, %7 over "
            "16 ms; resize event median %8 ms; %9 widget resizes per step; hidden-page resizes "
            "%10; "
            "engine re-renders %11")
                                 .arg(name, QGuiApplication::platformName())
                                 .arg(r.frames.steps)
                                 .arg(r.frames.medianMs, 0, 'f', 1)
                                 .arg(r.frames.p90Ms, 0, 'f', 1)
                                 .arg(r.frames.maxMs, 0, 'f', 1)
                                 .arg(r.frames.over16Ms)
                                 .arg(r.frames.medianResizeMs, 0, 'f', 1)
                                 .arg(r.resizesPerStep, 0, 'f', 0)
                                 .arg(r.hiddenPageResizes)
                                 .arg(r.outputSizeChanges);
    }

    void sweep_data()
    {
        QTest::addColumn<int>("config");
        QTest::newRow("dual viewers") << int(Config::Dual);
        QTest::newRow("single viewer") << int(Config::Single);
        QTest::newRow("clip loaded") << int(Config::Clip);
        QTest::newRow("keyframes docked") << int(Config::Keyframes);
    }

    /// An interactive-style resize 1280x720 -> 1920x1080 in 8 px steps at 60 Hz: per step the
    /// time from the window's resize event to its frame, the layout work, and what hidden pages
    /// and the viewers do. Thresholds are generous (software rendering, loaded CI machines);
    /// the structural checks are exact.
    void sweep()
    {
        QFETCH(int, config);
        const QString clip = test::fixture("h264_25fps.mp4");
        if (Config(config) == Config::Clip && !QFileInfo::exists(clip))
            QSKIP("media missing (scripts/make_test_media.sh)");
        const Sweep r = runSweep(Config(config), clip);
        qInfo().noquote() << QStringLiteral(
            "sweep on %1: %2 steps, frame median %3 ms / p90 %4 ms / max %5 ms, %6 over 16 ms; "
            "resize event median %7 ms; %8 widget resizes per step; viewer surface resizes per "
            "step <= %9; hidden-page resizes %10; engine re-renders %11")
                                 .arg(QGuiApplication::platformName())
                                 .arg(r.frames.steps)
                                 .arg(r.frames.medianMs, 0, 'f', 1)
                                 .arg(r.frames.p90Ms, 0, 'f', 1)
                                 .arg(r.frames.maxMs, 0, 'f', 1)
                                 .arg(r.frames.over16Ms)
                                 .arg(r.frames.medianResizeMs, 0, 'f', 1)
                                 .arg(r.resizesPerStep, 0, 'f', 0)
                                 .arg(r.maxSurfaceResizesPerStep)
                                 .arg(r.hiddenPageResizes)
                                 .arg(r.outputSizeChanges);
        QVERIFY(r.frames.steps > 0);
        QCOMPARE(r.hiddenPageResizes, 0);
        QVERIFY2(r.maxSurfaceResizesPerStep <= 1, "a viewer is laid out twice per resize step");
        if (r.frames.medianMs < 60)
            QVERIFY2(r.outputSizeChanges <= 2, "the viewers re-render while resizing");
        QVERIFY2(r.frames.medianMs < 150, "a resize step takes far longer than a frame");
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

int main(int argc, char** argv)
{
    // The application's OpenGL setup (src/main.cpp), so the viewers on xcb / wayland are the
    // OpenGL ones with shared contexts, as in the real app.
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    ProfilingApplication app(argc, argv);
    liveresize::adjustSwapInterval(); // as main(); KK_SWAP_INTERVAL=1 for the old behaviour
    TestResizeBench test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_resizebench.moc"
