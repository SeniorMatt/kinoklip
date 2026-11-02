// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen UI tests of the viewers: timeline viewer showing the program engine's frames
// (raster path), header/timecode/marks/zoom, source viewer loading a clip with its own marks
// (I/O keys claimed from the timeline while focused), transport routing to the active viewer,
// full-screen viewer, and F9 inserting the source viewer's marked range (EditPage wiring).
#include "../media/MediaTestUtil.h"
#include "app/pages/EditPage.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/viewer/PlaybackResolution.h"
#include "ui/viewer/ScrubBar.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"
#include "ui/viewer/ViewerTransport.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTest>

#include <memory>

using namespace kk;

namespace {

/// What main() creates (project, actions, program engine without sound).
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    std::unique_ptr<PlaybackEngine> engine;
    AppContext ctx;

    explicit Fixture(bool withEngine = true)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        if (withEngine) {
            PlaybackEngine::Options options;
            options.noAudio = true;
            engine = std::make_unique<PlaybackEngine>(std::move(options));
            engine->setMediaPool(project.mediaPool());
            engine->setTimeline(project.timeline());
            ctx.playback = engine.get();
        }
    }

    /// 320x180 timeline with a red solid over 100 frames.
    void solidTimeline()
    {
        TimelineData tl = makeDefaultTimeline(QStringLiteral("Demo"), {25, 1}, 320, 180);
        ClipData red;
        red.id = tl.allocateId();
        red.kind = ClipKind::Solid;
        red.solidColor = Qt::red;
        red.duration = 100;
        tl.videoTracks[0].clips.push_back(red);
        project.timeline()->setData(tl);
    }
};

QColor centreOf(QWidget* w, const QRect& r)
{
    return w->grab().toImage().pixelColor(r.center());
}

} // namespace

class TestViewerPanel : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        initResources();
        // Private settings (the playback resolution is persisted).
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_viewerpanel"));
        QSettings().clear();
    }

    void timelineViewerShowsProgram()
    {
        Fixture f;
        f.solidTimeline();
        ViewerPanel viewer(ViewerPanel::Mode::Timeline, f.ctx);
        viewer.resize(640, 480);
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        // Resolve's viewer header shows only the name of what is loaded.
        QVERIFY(viewer.title().contains(QLatin1String("Demo")));
        QCOMPARE(viewer.engine(), f.engine.get());
        QWidget* surface = viewer.surface()->widget();
        QVERIFY(QTest::qWaitFor(
            [&] { return centreOf(surface, viewer.surface()->pictureRect()).red() > 200; }, 3000));
        // The engine renders for the viewer's picture size (raster: images).
        QVERIFY(f.engine->outputSize().width() <= viewer.surface()->pictureRect().width() + 1);

        f.project.timeline()->setPlayhead(25);
        QTRY_COMPARE(viewer.findChild<QLabel*>(QStringLiteral("TimecodeLabel"))->text(),
            QStringLiteral("00:00:01:00"));
        auto* scrub = viewer.findChild<ScrubBar*>();
        QCOMPARE(scrub->duration(), int64_t(100));
        QCOMPARE(scrub->position(), int64_t(25));

        // Marks show in the scrub bar and the marks label.
        TimelineData d = f.project.timeline()->data();
        d.markIn = 10;
        d.markOut = 60;
        f.project.timeline()->setData(d);
        QCOMPARE(scrub->markIn(), int64_t(10));
        QCOMPARE(scrub->markOut(), int64_t(60));
        const QString marks = viewer.findChild<QLabel*>(QStringLiteral("ViewerMarks"))->text();
        QVERIFY2(marks.contains(QLatin1String("00:00:02:00")), qPrintable(marks)); // 50 frames
        // Scrubbing through the bar moves the program playhead.
        QTest::mouseClick(
            scrub, Qt::LeftButton, {}, QPoint(int(scrub->xOf(80)) + 1, scrub->height() / 2));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(80));
    }

    void zoomModes()
    {
        Fixture f;
        f.solidTimeline();
        ViewerPanel viewer(ViewerPanel::Mode::Timeline, f.ctx);
        viewer.resize(900, 600);
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        const qreal dpr = viewer.surface()->widget()->devicePixelRatioF();
        viewer.setZoom(ViewerPanel::Zoom::Percent100);
        QCOMPARE(viewer.surface()->pictureRect().size(), QSize(int(320 / dpr), int(180 / dpr)));
        QCOMPARE(viewer.findChild<QComboBox*>(QStringLiteral("ViewerZoom"))->currentIndex(), 2);
        viewer.setZoom(ViewerPanel::Zoom::Percent200);
        QCOMPARE(viewer.surface()->pictureRect().width(), int(640 / dpr));
        QCOMPARE(f.engine->outputSize(), QSize(320, 180)); // never above timeline resolution
        viewer.setZoom(ViewerPanel::Zoom::Fit);
        QVERIFY(viewer.surface()->pictureRect().width() > 640);
        // The resolution box drives the engine and the shared setting (menu, other viewers).
        auto* box = viewer.findChild<QComboBox*>(QStringLiteral("ViewerQuality"));
        QCOMPARE(box->currentText(), QStringLiteral("Full Res"));
        QCOMPARE(f.engine->qualityMode(), PlaybackEngine::QualityMode::Full);
        box->setCurrentIndex(box->findData(int(PlaybackEngine::QualityMode::Half)));
        QCOMPARE(f.engine->qualityMode(), PlaybackEngine::QualityMode::Half);
        QCOMPARE(PlaybackResolution::instance().mode(), PlaybackEngine::QualityMode::Half);
        QCOMPARE(prefs::previewQuality(), QStringLiteral("half"));
        ViewerPanel other(ViewerPanel::Mode::Timeline, f.ctx);
        auto* otherBox = other.findChild<QComboBox*>(QStringLiteral("ViewerQuality"));
        QCOMPARE(otherBox->currentData().toInt(), int(PlaybackEngine::QualityMode::Half));
        PlaybackResolution::instance().setMode(PlaybackEngine::QualityMode::Quarter); // menu
        QCOMPARE(box->currentText(), QStringLiteral("Quarter Res"));
        QCOMPARE(otherBox->currentText(), QStringLiteral("Quarter Res"));
        QCOMPARE(f.engine->qualityMode(), PlaybackEngine::QualityMode::Quarter);
        PlaybackResolution::instance().setMode(PlaybackEngine::QualityMode::Full);
    }

    void timelineViewerWithoutEngine()
    {
        Fixture f(false);
        f.solidTimeline();
        ViewerPanel viewer(ViewerPanel::Mode::Timeline, f.ctx);
        QVERIFY(!viewer.engine());
        f.project.timeline()->setPlayhead(50);
        QCOMPARE(viewer.findChild<QLabel*>(QStringLiteral("TimecodeLabel"))->text(),
            QStringLiteral("00:00:02:00"));
    }

    void sourceViewerLoadsClipAndMarks()
    {
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
        Fixture f;
        const QString id
            = f.project.importMedia({test::fixture("h264_25fps.mp4")}).importedIds.value(0);
        QVERIFY(!id.isEmpty());
        QMainWindow window; // shortcuts need a window with the actions
        auto* panel = new ViewerPanel(ViewerPanel::Mode::Source, f.ctx);
        ViewerPanel& viewer = *panel;
        window.addActions(f.actions.actions());
        window.setCentralWidget(panel);
        window.resize(640, 480);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(!viewer.sourcePlayer()); // created on first use
        viewer.setSourceMedia(id);
        QVERIFY(viewer.sourcePlayer());
        QVERIFY(viewer.title().contains(QLatin1String("h264_25fps")));
        QWidget* surface = viewer.surface()->widget();
        // testsrc2 picture: not black once decoded.
        QVERIFY(QTest::qWaitFor(
            [&] {
                const QColor c = centreOf(surface, viewer.surface()->pictureRect());
                return c.red() + c.green() + c.blue() > 60;
            },
            5000));
        PlaybackEngine* e = viewer.engine();
        QVERIFY(e && e != f.engine.get());
        QCOMPARE(e->duration(), int64_t(250)); // 10 s at 25 fps

        // I / O with focus in the source viewer mark the clip, not the timeline.
        viewer.surface()->widget()->setFocus();
        e->seek(25);
        QTest::keyClick(viewer.surface()->widget(), Qt::Key_I);
        e->seek(74);
        QTest::keyClick(viewer.surface()->widget(), Qt::Key_O);
        QCOMPARE(viewer.sourcePlayer()->markIn(), int64_t(25));
        QCOMPARE(viewer.sourcePlayer()->markOut(), int64_t(75));
        QCOMPARE(f.project.timeline()->data().markIn, int64_t(-1));
        const auto range = viewer.sourceSelection();
        QVERIFY(range);
        QCOMPARE(range->inUs, int64_t(1'000'000));
        QCOMPARE(range->outUs, int64_t(3'000'000));
        // Home goes to the clip start; Shift+I back to the in mark.
        QTest::keyClick(viewer.surface()->widget(), Qt::Key_Home);
        QCOMPARE(e->position(), int64_t(0));
        QTest::keyClick(viewer.surface()->widget(), Qt::Key_I, Qt::ShiftModifier);
        QCOMPARE(e->position(), int64_t(25));
        // Marks are remembered per clip.
        viewer.setSourceMedia(QString());
        QVERIFY(viewer.sourceSelection() == std::nullopt);
        viewer.setSourceMedia(id);
        QCOMPARE(viewer.sourcePlayer()->markIn(), int64_t(25));
        // Removing the item from the pool unloads it.
        f.project.mediaPool()->removeItem(id);
        QVERIFY(viewer.sourceMedia().isEmpty());
    }

    void transportFollowsActiveViewer()
    {
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
        Fixture f;
        f.solidTimeline();
        const QString id
            = f.project.importMedia({test::fixture("h264_25fps.mp4")}).importedIds.value(0);
        QWidget host;
        auto* layout = new QHBoxLayout(&host);
        auto* source = new ViewerPanel(ViewerPanel::Mode::Source, f.ctx);
        auto* program = new ViewerPanel(ViewerPanel::Mode::Timeline, f.ctx);
        layout->addWidget(source);
        layout->addWidget(program);
        host.addActions(f.actions.actions());
        host.resize(1000, 400);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        source->setSourceMedia(id);
        ViewerTransport transport(f.ctx);
        transport.connectActions();

        program->activate();
        QCOMPARE(transport.targetEngine(), f.engine.get());
        f.actions.action(QStringLiteral("transport.next_frame"))->trigger();
        QCOMPARE(f.engine->position(), int64_t(1));

        source->activate();
        QCOMPARE(transport.targetEngine(), source->engine());
        f.actions.action(QStringLiteral("transport.forward_1s"))->trigger();
        QCOMPARE(source->engine()->position(), int64_t(25));
        QCOMPARE(f.engine->position(), int64_t(1)); // untouched
        // Space plays the active (source) viewer; K stops it.
        f.actions.action(QStringLiteral("transport.play_pause"))->trigger();
        QVERIFY(source->engine()->isPlaying());
        QVERIFY(!f.engine->isPlaying());
        f.actions.action(QStringLiteral("transport.shuttle_stop"))->trigger();
        QVERIFY(!source->engine()->isPlaying());
        // L twice = 2x, J = reverse 1x.
        f.actions.action(QStringLiteral("transport.shuttle_forward"))->trigger();
        f.actions.action(QStringLiteral("transport.shuttle_forward"))->trigger();
        QCOMPARE(source->engine()->rate(), 2.0);
        f.actions.action(QStringLiteral("transport.shuttle_reverse"))->trigger();
        QCOMPARE(source->engine()->rate(), -1.0);
        source->engine()->stop();
        // The loop toggle applies to every viewer's engine.
        f.actions.action(QStringLiteral("transport.loop"))->trigger();
        QVERIFY(source->engine()->loop());
        QVERIFY(f.engine->loop());
        f.actions.action(QStringLiteral("transport.loop"))->trigger();

        // Focus in a tagged widget (the timeline panel) targets the program viewer.
        auto* timelineStandIn = new QWidget;
        timelineStandIn->setFocusPolicy(Qt::StrongFocus);
        timelineStandIn->setProperty("kkProgramFocus", true);
        layout->addWidget(timelineStandIn);
        timelineStandIn->show();
        host.activateWindow(); // focus changes need an active window (xvfb has no WM)
        QVERIFY(QTest::qWaitForWindowActive(&host));
        timelineStandIn->clearFocus(); // activation may already have focused it
        source->activate();
        timelineStandIn->setFocus();
        QTRY_COMPARE(ViewerPanel::activeViewer(), program);

        // Full-screen viewer (Ctrl+F action): opens a picture-only program viewer.
        f.actions.action(QStringLiteral("view.fullscreen_viewer"))->trigger();
        QVERIFY(transport.isFullScreenViewerOpen());
        QTRY_VERIFY2(transport.targetEngine() == f.engine.get(),
            qPrintable(QStringLiteral("target viewer %1")
                           .arg(transport.targetViewer() ? transport.targetViewer()->objectName()
                                                         : QStringLiteral("none"))));
        f.actions.action(QStringLiteral("view.fullscreen_viewer"))->trigger();
        QTRY_VERIFY(!transport.isFullScreenViewerOpen());
    }

    /// F9 on the Edit page inserts the source viewer's marked range at the playhead.
    void insertMarkedSourceRange()
    {
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
        Fixture f;
        f.project.newProject();
        const QString id
            = f.project.importMedia({test::fixture("h264_25fps.mp4")}).importedIds.value(0);
        QMainWindow window;
        auto* edit = new EditPage(f.ctx);
        EditPage& page = *edit;
        window.addActions(f.actions.actions());
        window.setCentralWidget(edit);
        window.resize(1600, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        ViewerPanel* source = page.sourceViewer();
        source->setSourceMedia(id);
        source->sourcePlayer()->setMarks(50, 100); // 2 s .. 4 s of the clip
        f.actions.action(QStringLiteral("edit.insert"))->trigger();
        const TimelineData& tl = f.project.timeline()->data();
        QVERIFY(!tl.videoTracks.empty());
        QCOMPARE(int(tl.videoTracks[0].clips.size()), 1);
        const ClipData& clip = tl.videoTracks[0].clips[0];
        QCOMPARE(clip.mediaId, id);
        QCOMPARE(clip.sourceInUs, int64_t(2'000'000));
        QCOMPARE(clip.duration, int64_t(50)); // 2 s at the 25 fps timeline
        QVERIFY(!tl.audioTracks.empty() && !tl.audioTracks[0].clips.empty()); // linked audio
        // The program viewer now shows it (after the playhead moved to the end of the clip).
        QCOMPARE(f.project.timeline()->playhead(), int64_t(50));
    }
};

int main(int argc, char** argv)
{
    qputenv("KK_NO_AUDIO", "1"); // source players use the default output: keep the tests silent
    // Same GL set-up as main(): shared contexts let GL viewers draw compositor textures.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    TestViewerPanel test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_viewerpanel.moc"
