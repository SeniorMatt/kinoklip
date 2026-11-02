// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end user flow in the real main window (offscreen / xvfb): import -> drag to the
// timeline -> trim / blade / move -> title + transition + keyframes -> play -> Deliver with
// the YouTube 1080p preset -> render -> probe the file. Everything goes through the widgets
// and actions a user would touch (drops, mouse gestures, shortcuts' actions, inspector
// buttons, Deliver page), so integration breaks between modules show up here.
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "export/RenderQueue.h"
#include "media/MediaInfo.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/deliver/RenderSettingsPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kV2 {TrackType::Video, 1};
constexpr TrackRef kA1 {TrackType::Audio, 0};

/// Everything main() creates (program engine without a sound device).
struct App
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    std::unique_ptr<PlaybackEngine> playback;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    App()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        playback = std::make_unique<PlaybackEngine>(std::move(options));
        playback->setMediaPool(project.mediaPool());
        playback->setTimeline(project.timeline());
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = playback.get();
        MainWindowOptions wo;
        wo.autosave = false;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, wo);
    }
    void trigger(const char* id) { actions.action(QLatin1String(id))->trigger(); }
    const TimelineData& tl() const { return project.timeline()->data(); }
};

/// Sends drag-enter / move / drop of @p mime at @p pos to @p target.
bool dropOn(QWidget* target, QMimeData* mime, QPoint pos)
{
    QDragEnterEvent enter(pos, Qt::CopyAction, mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(target, &enter);
    QDragMoveEvent move(pos, Qt::CopyAction, mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(target, &move);
    QDropEvent drop(pos, Qt::CopyAction, mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(target, &drop);
    return drop.isAccepted();
}

QString layout(const TimelineData& d, TrackRef ref)
{
    QStringList parts;
    for (const ClipData& c : d.track(ref)->clips)
        parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
    return parts.join(QLatin1Char(' '));
}

} // namespace

class TestE2EFlow : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_e2eflow"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void importEditPlayDeliver()
    {
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        App app;
        app.window->resize(1600, 1000);
        app.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(app.window.get()));
        EditPage* edit = app.window->editPage();
        QVERIFY(edit);
        // The window opens on the Edit page even without a restored layout.
        QCOMPARE(app.window->currentPageId(), QStringLiteral("edit"));
        QVERIFY(edit->isVisible());
        QVERIFY(edit->timeline()->timelineWidget()->viewport()->width() > 600);

        // ---- Import (10 s 1280x720 25 fps H.264 + AAC) ----
        const ImportResult imported = app.project.importMedia({test::fixture("h264_25fps.mp4")});
        QCOMPARE(imported.importedIds.size(), 1);
        const QString mediaId = imported.importedIds.front();

        // ---- Drag from the media pool onto V1 at frame 0 ----
        TimelineWidget* tw = edit->timeline()->timelineWidget();
        TimelineController* tc = edit->timeline()->controller();
        QWidget* vp = tw->viewport();
        tw->setPixelsPerFrame(4.0, timelinemetrics::kHeaderWidth);
        tw->scrollToFrame(0);
        auto at = [&](double frame, TrackRef ref) {
            const TimelineLayout& L = tw->layout();
            return QPoint(
                int(std::lround(L.frameToX(frame))), L.rowRect(L.rowOf(ref)).center().y());
        };
        {
            QMimeData mime;
            mime.setData(QLatin1String(mime::MediaItems), mediaId.toUtf8());
            QVERIFY(dropOn(vp, &mime, at(0, kV1)));
        }
        QCOMPARE(layout(app.tl(), kV1), QStringLiteral("0:250"));
        QCOMPARE(layout(app.tl(), kA1), QStringLiteral("0:250"));
        const quint64 v1 = app.tl().videoTracks[0].clips.front().id;
        QVERIFY(app.tl().videoTracks[0].clips.front().linkGroup != 0);

        // ---- Trim the tail by 50 frames (selection tool, linked audio follows) ----
        const QPoint tail = at(250, kV1) - QPoint(2, 0);
        QTest::mousePress(vp, Qt::LeftButton, {}, tail);
        for (int i = 1; i <= 5; ++i)
            QTest::mouseMove(vp, tail - QPoint(40 * i, 0));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, tail - QPoint(200, 0));
        QCOMPARE(layout(app.tl(), kV1), QStringLiteral("0:200"));
        QCOMPARE(layout(app.tl(), kA1), QStringLiteral("0:200"));

        // ---- Blade at the playhead (Ctrl+B), then move the right part 25 frames later ----
        app.project.timeline()->setPlayhead(100);
        app.selection.clear();
        app.trigger("edit.split");
        QCOMPARE(layout(app.tl(), kV1), QStringLiteral("0:100 100:100"));
        QCOMPARE(layout(app.tl(), kA1), QStringLiteral("0:100 100:100"));
        const QPoint body = at(150, kV1);
        QTest::mousePress(vp, Qt::LeftButton, {}, body);
        for (int i = 1; i <= 5; ++i)
            QTest::mouseMove(vp, body + QPoint(20 * i, 0));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, body + QPoint(100, 0));
        QCOMPARE(layout(app.tl(), kV1), QStringLiteral("0:100 125:100"));
        QCOMPARE(layout(app.tl(), kA1), QStringLiteral("0:100 125:100"));

        // ---- Close the gap: click it, Backspace ----
        QTest::mouseClick(vp, Qt::LeftButton, {}, at(112, kV1));
        QVERIFY(tc->selectedGap().has_value());
        app.trigger("edit.delete");
        QCOMPARE(layout(app.tl(), kV1), QStringLiteral("0:100 100:100"));
        QCOMPARE(layout(app.tl(), kA1), QStringLiteral("0:100 100:100"));

        // ---- Title on V2 from the effects library drag, transition at the cut (Ctrl+T) ----
        {
            QMimeData mime;
            mime.setData(QLatin1String(mime::Effect), QByteArrayLiteral("text_title"));
            const QPoint p = at(20, kV2);
            const TimelineHit hit = tw->layout().hitTest(app.tl(), p);
            QVERIFY2(dropOn(vp, &mime, p),
                qPrintable(QStringLiteral("area %1 row %2 x %3 y %4 vp %5x%6")
                               .arg(int(hit.area))
                               .arg(hit.row)
                               .arg(p.x())
                               .arg(p.y())
                               .arg(vp->width())
                               .arg(vp->height())));
        }
        QCOMPARE(app.tl().videoTracks[1].clips.size(), size_t(1));
        const ClipData title = app.tl().videoTracks[1].clips.front();
        QCOMPARE(title.kind, ClipKind::Title);
        QCOMPARE(title.duration, int64_t(125)); // spec 14.1
        app.selection.clear();
        app.project.timeline()->setPlayhead(98);
        app.trigger("timeline.add_transition");
        const ClipData& right = app.tl().videoTracks[0].clips[1];
        QVERIFY(right.transitionIn.has_value());
        QCOMPARE(right.transitionIn->duration, int64_t(25));

        // ---- Keyframes on the title's opacity through the inspector diamond ----
        app.selection.selectClip(title.id);
        InspectorPanel* inspector = edit->inspector();
        QVERIFY(inspector);
        app.project.timeline()->setPlayhead(title.start);
        auto* diamond
            = inspector->findChild<KeyframeButton*>(QStringLiteral("keyframe:") + keys::Opacity);
        QVERIFY(diamond);
        QTRY_VERIFY(diamond->isEnabled());
        diamond->click();
        app.project.timeline()->setPlayhead(title.start + 50);
        QTRY_VERIFY(diamond->state() == KeyframeButton::State::Animated);
        diamond->click();
        // Make the second key different so the animation is visible.
        const double full = ops::clipById(app.tl(), title.id)->transform.value(keys::Opacity, 0);
        app.project.beginEdit(QStringLiteral("value"));
        ops::clipById(app.project.timeline()->mutableData(), title.id)
            ->transform.animated(keys::Opacity, full)
            .setKey(50, full / 4);
        app.project.endEdit();
        const ClipData* t = ops::clipById(app.tl(), title.id);
        QCOMPARE(t->transform.values.value(keys::Opacity).keys.size(), 2);
        const double mid = t->transform.value(keys::Opacity, 25);
        QVERIFY(mid < full && mid > full / 4);
        QVERIFY(ops::checkInvariants(app.tl()));

        // ---- Play: the engine advances and the timeline viewer receives frames ----
        app.selection.clear();
        app.project.timeline()->setPlayhead(0);
        QSignalSpy frames(app.playback.get(), &PlaybackEngine::frameReady);
        app.trigger("transport.play_pause");
        QTRY_VERIFY_WITH_TIMEOUT(app.project.timeline()->playhead() >= 25, 10000);
        app.trigger("transport.play_pause");
        QVERIFY(!app.playback->isPlaying());
        QVERIFY(frames.count() >= 5);
        QVERIFY(app.playback->stats().presented >= 5);

        // ---- Deliver: YouTube 1080p job, render, probe ----
        QVERIFY(app.window->setCurrentPage(QStringLiteral("deliver")));
        DeliverPage* deliver = app.window->deliverPage();
        RenderSettingsPanel* settings = deliver->settingsPanel();
        settings->applyPreset(QStringLiteral("youtube_1080p"));
        QTemporaryDir dir;
        ExportSettings s = settings->settings();
        s.outputPath = dir.filePath(QStringLiteral("e2e.mp4"));
        settings->setSettings(s);
        QCOMPARE(settings->settings().outputSize(app.tl()), QSize(1920, 1080));
        RenderQueue* queue = deliver->renderQueue();
        const quint64 job = deliver->addCurrentToQueue(false);
        QVERIFY(job != 0);
        queue->startAll();
        QVERIFY(queue->waitForIdle(300000));
        QVERIFY2(queue->job(job)->status == RenderQueue::Status::Done,
            qPrintable(queue->job(job)->result.error));
        const media::MediaInfo info = media::probe(s.outputPath);
        QVERIFY(info.valid);
        QVERIFY(info.hasVideo && info.hasAudio);
        QCOMPARE(info.width, 1920);
        QCOMPARE(info.height, 1080);
        // 200 frames at 25 fps = 8 s (container rounding: allow 0.1 s).
        QVERIFY2(std::abs(info.durationUs - 8'000'000) < 100'000,
            qPrintable(QString::number(info.durationUs)));
    }
};

QTEST_MAIN(TestE2EFlow)
#include "test_e2eflow.moc"
