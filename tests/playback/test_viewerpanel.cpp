// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen UI tests of the viewers: timeline viewer showing the program engine's frames
// (raster path), header/timecode/marks/zoom, source viewer loading a clip with its own marks
// (I/O keys claimed from the timeline while focused), transport routing to the active viewer,
// full-screen viewer, and F9 inserting the source viewer's marked range (EditPage wiring).
#include "../core/ClassicTimeline.h"
#include "../media/MediaTestUtil.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/viewer/PlaybackResolution.h"
#include "ui/viewer/ScrubBar.h"
#include "ui/viewer/TransformOverlay.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerSurface.h"
#include "ui/viewer/ViewerTransport.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTest>
#include <QUndoStack>

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
        kk::test::useClassicTimeline(project); // the tests count frames at 25 fps
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
        TimelineData tl = makeDefaultTimeline(QStringLiteral("Demo"), {25, 1}, 320, 180, 2, 2);
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

    // 41.22: zoomed and panned, the raster surface draws only the visible part of the frame,
    // pixel for pixel where the picture mapping says.
    void zoomedRasterDrawsVisiblePart()
    {
        Fixture f(false);
        f.solidTimeline();
        ViewerPanel viewer(ViewerPanel::Mode::Timeline, f.ctx);
        viewer.resize(700, 500);
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        QImage frame(320, 180, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < frame.height(); ++y)
            for (int x = 0; x < frame.width(); ++x)
                frame.setPixel(x, y, qRgb(x % 256, y, (x / 256) * 128));
        viewer.showFrame(frame);
        viewer.setZoomFactor(4.0);
        viewer.setPan({-150, 90});
        QCoreApplication::processEvents();
        QWidget* surface = viewer.surface()->widget();
        const QRect pic = viewer.surface()->pictureRect();
        QVERIFY(!surface->rect().contains(pic)); // partly outside: the clipped path
        const QImage shown = surface->grab().toImage();
        const qreal dpr = surface->devicePixelRatioF();
        const double scale = pic.width() / 320.0;
        QCOMPARE(scale, 4.0 / dpr);
        int checked = 0;
        // The centres of frame pixels that land inside the widget.
        for (const QPoint px : {QPoint(130, 90), QPoint(150, 30), QPoint(200, 60), QPoint(250, 80),
                 QPoint(300, 170)}) {
            const QPoint w(
                int(pic.x() + (px.x() + 0.5) * scale), int(pic.y() + (px.y() + 0.5) * scale));
            if (!surface->rect().contains(w))
                continue;
            QCOMPARE(QColor(shown.pixel(QPoint(int(w.x() * dpr), int(w.y() * dpr)))).rgb(),
                QColor(frame.pixel(px)).rgb());
            ++checked;
        }
        QVERIFY(checked >= 3);

        // Paint cost of a 4K frame at 400 % against Fit (reported, not asserted: the host may
        // be loaded). Scaling only the visible part keeps the zoomed paint about as cheap.
        QImage uhd(3840, 2160, QImage::Format_ARGB32_Premultiplied);
        uhd.fill(QColor(0x40, 0x80, 0xc0));
        viewer.surface()->setAspect(uhd.size()); // a UHD timeline
        viewer.showFrame(uhd);
        auto paintMs = [surface] {
            QElapsedTimer t;
            t.start();
            for (int i = 0; i < 5; ++i)
                surface->repaint();
            return t.elapsed() / 5.0;
        };
        viewer.zoomToFit();
        const double fitMs = paintMs();
        viewer.setZoomFactor(4.0);
        const double zoomedMs = paintMs();
        qInfo("4K raster paint: Fit %.1f ms, 400 %% %.1f ms", fitMs, zoomedMs);
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
    /// Polish round 2 (offline media): the timeline viewer says "Media Offline" with the clip
    /// name while the top clip at the playhead has no file; a solid there clears it; the
    /// source viewer says it for an offline source clip.
    void mediaOfflineNotice()
    {
        Fixture f;
        f.solidTimeline();
        MediaItem item;
        item.path = QStringLiteral("/nonexistent/moved.mp4");
        item.name = QStringLiteral("moved.mp4");
        item.info.valid = true;
        item.info.hasVideo = true;
        item.info.durationUs = 10'000'000;
        const QString id = f.project.mediaPool()->addItem(item);
        QVERIFY(f.project.mediaPool()->isOffline(id));
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = id;
        clip.name = QStringLiteral("moved.mp4");
        clip.start = 200;
        clip.duration = 50;
        clip.sourceDurationUs = 10'000'000;
        f.project.beginEdit(QStringLiteral("place"));
        editops::overwriteClip(f.project.timeline()->mutableData(), {TrackType::Video, 0}, clip);
        f.project.endEdit();
        ViewerPanel program(ViewerPanel::Mode::Timeline, f.ctx);
        program.resize(480, 320);
        program.show();
        QVERIFY(QTest::qWaitForWindowExposed(&program));
        QVERIFY(program.offlineNotice());
        QVERIFY(!program.offlineNotice()->isVisible()); // the solid at frame 0
        f.project.timeline()->setPlayhead(220);
        QTRY_VERIFY(program.offlineNotice()->isVisible());
        QVERIFY(program.offlineNotice()->text().contains(QLatin1String("Media Offline")));
        QVERIFY(program.offlineNotice()->text().contains(QLatin1String("moved.mp4")));
        f.project.timeline()->setPlayhead(10);
        QTRY_VERIFY(!program.offlineNotice()->isVisible());
        ViewerPanel source(ViewerPanel::Mode::Source, f.ctx);
        source.show();
        source.setSourceMedia(id);
        source.updateOfflineNotice();
        QVERIFY(source.offlineNotice()->isVisible());
        // Round 2 audit C1: the viewers have a context menu of registry commands.
        std::unique_ptr<QMenu> menu(program.buildContextMenu(nullptr));
        QVERIFY(menu->actions().contains(f.actions.action(QStringLiteral("transport.play_pause"))));
        QVERIFY(menu->actions().contains(f.actions.action(QStringLiteral("mark.in"))));
        QVERIFY(menu->actions().contains(f.actions.action(QStringLiteral("clip.match_frame"))));
        std::unique_ptr<QMenu> sourceMenu(source.buildContextMenu(nullptr));
        QVERIFY(
            !sourceMenu->actions().contains(f.actions.action(QStringLiteral("clip.match_frame"))));
    }

    /// Polish round 2 (titles): with the on-screen controls on Transform, dragging in the
    /// timeline viewer moves the current clip (Position X / Y in timeline pixels), one undo
    /// step per drag; Shift keeps one axis.
    void onScreenTransformMovesTheClip()
    {
        Fixture f;
        f.solidTimeline(); // 320x180, a solid over frames 0..99 on V1
        const quint64 id = f.project.timeline()->data().videoTracks[0].clips[0].id;
        f.selection.setClips({id});
        ViewerPanel program(ViewerPanel::Mode::Timeline, f.ctx);
        program.resize(700, 500);
        program.show();
        QVERIFY(QTest::qWaitForWindowExposed(&program));
        TransformOverlay* overlay = program.transformOverlay();
        QVERIFY(overlay);
        QVERIFY(!overlay->isEditing()); // the controls are off
        f.actions.action(QStringLiteral("view.onscreen_transform"))->setChecked(true);
        QTRY_VERIFY(overlay->isEditing());
        const auto map = overlay->frameToWidget();
        QVERIFY(map);
        // Off the centre: the anchor cross there moves the anchor point (spec 24.11).
        const QPoint from = map->map(QPointF(130, 110)).toPoint();
        const QPoint to = map->map(QPointF(170, 90)).toPoint(); // +40 px right, 20 px up
        const int steps = f.project.undoStack()->count();
        QTest::mousePress(overlay, Qt::LeftButton, {}, from);
        QTest::mouseMove(overlay, (from + to) / 2);
        QTest::mouseMove(overlay, to);
        QTest::mouseRelease(overlay, Qt::LeftButton, {}, to);
        const ClipData* clip = editops::clipById(f.project.timeline()->data(), id);
        QVERIFY(std::abs(clip->transform.value(keys::PosX, 0) - 40) <= 2);
        QVERIFY(std::abs(clip->transform.value(keys::PosY, 0) + 20) <= 2);
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Move Clip"));
        // Shift: one axis only.
        QTest::mousePress(overlay, Qt::LeftButton, {}, from);
        QTest::mouseMove(overlay, to, 0);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), overlay->mapToGlobal(QPointF(to)),
            Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
        QCoreApplication::sendEvent(overlay, &move);
        QTest::mouseRelease(overlay, Qt::LeftButton, Qt::ShiftModifier, to);
        clip = editops::clipById(f.project.timeline()->data(), id);
        QVERIFY(std::abs(clip->transform.value(keys::PosX, 0) - 80) <= 3);
        QVERIFY(std::abs(clip->transform.value(keys::PosY, 0) + 20) <= 2); // unchanged
        // The Mask mode turns Transform off (one mode at a time).
        f.actions.action(QStringLiteral("view.onscreen_masks"))->setChecked(true);
        f.actions.action(QStringLiteral("view.onscreen_transform"))->setChecked(false);
        QTRY_VERIFY(!overlay->isEditing());
        f.actions.action(QStringLiteral("view.onscreen_masks"))->setChecked(false);
    }

    void insertMarkedSourceRange()
    {
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
        Fixture f;
        f.project.newProject();
        kk::test::useClassicTimeline(f.project); // a 25 fps timeline
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
