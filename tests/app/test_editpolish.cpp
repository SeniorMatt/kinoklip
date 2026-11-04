// SPDX-License-Identifier: GPL-3.0-or-later
// The polish-v3 editing aids in the real main window (docs/polish-v3.md): media dropped on
// the timeline viewer's edit overlay (spec 4.14), typed timecodes with the timeline focused
// (spec 2.10), the two-up trim view and the drag offset label while trimming (spec 8.15,
// 21.9). With KK_SCREENSHOT_DIR set, each state is also saved as a PNG for review.
#include "../core/ClassicTimeline.h"
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/EditOverlay.h"
#include "ui/viewer/TrimTwoUpView.h"
#include "ui/viewer/ViewerPanel.h"

#include <QApplication>
#include <QLabel>
#include <QMimeData>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace kk;
using namespace kk::test;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};

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
        kk::test::useClassicTimeline(project); // the tests count frames at 25 fps
        ctx.playback = playback.get();
        MainWindowOptions wo;
        wo.autosave = false;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, wo);
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
    EditPage* page() const { return window->editPage(); }
    TimelineWidget* timeline() const { return page()->timeline()->timelineWidget(); }
    ViewerPanel* viewer() const { return page()->timelineViewer(); }
    const TimelineData& tl() const { return project.timeline()->data(); }
};

QString layout(const TimelineData& d, TrackRef ref)
{
    QStringList parts;
    for (const ClipData& c : d.track(ref)->clips)
        parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
    return parts.join(QLatin1Char(' '));
}

/// Saves @p widget as <KK_SCREENSHOT_DIR>/<name>.png when the variable is set.
void shot(QWidget* widget, const char* name)
{
    const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    QCoreApplication::processEvents();
    widget->grab().save(QDir(dir).filePath(QLatin1String(name) + QStringLiteral(".png")));
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
using DragPos = QPointF;
#else
using DragPos = QPoint;
#endif

} // namespace

class TestEditPolish : public QObject
{
    Q_OBJECT

    QString m_skip;
    QString m_media;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_editpolish"));
        QSettings().clear();
        initResources();
        if (!ensureFixtures(&m_skip))
            qWarning("%s", qPrintable(m_skip));
        m_media = fixture("h264_25fps.mp4");
    }

    /// Dragging a media pool item over the timeline viewer shows the edit tiles; dropping on
    /// Overwrite places it at the playhead, on Insert it ripples (spec 4.14).
    void spec4_14_dropOnTheViewerOverlay()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        App app;
        const ImportResult imported = app.project.importMedia({m_media});
        QCOMPARE(imported.importedIds.size(), 1);
        const QString id = imported.importedIds.front();
        ViewerPanel* viewer = app.viewer();
        EditOverlay* overlay = viewer->editOverlay();
        QVERIFY(overlay);
        QCOMPARE(overlay->tiles().size(), 7);
        QCOMPARE(overlay->tiles()[1].actionId, QStringLiteral("edit.overwrite"));
        QCOMPARE(overlay->tiles()[1].key, QStringLiteral("F10"));

        QMimeData mime;
        mime.setData(QLatin1String(mime::MediaItems), id.toUtf8());
        const auto tileCentre = [&](int i) {
            return overlay->mapTo(viewer, overlay->tileRect(i).center());
        };
        const DragPos at(tileCentre(1));
        QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(viewer, &enter);
        QVERIFY(enter.isAccepted());
        QVERIFY(overlay->isVisible());
        QDragMoveEvent move(at, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(viewer, &move);
        QCOMPARE(overlay->hovered(), 1);
        shot(app.window.get(), "edit_overlay");
        QDropEvent drop(at, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(viewer, &drop);
        QVERIFY(drop.isAccepted());
        QVERIFY(!overlay->isVisible());
        QCOMPARE(layout(app.tl(), kV1), QStringLiteral("0:250"));
        QCOMPARE(app.project.timeline()->playhead(), int64_t(250));

        // Insert at frame 100 ripples the rest.
        app.project.timeline()->setPlayhead(100);
        const DragPos ins(tileCentre(0));
        QDragEnterEvent enter2(ins, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(viewer, &enter2);
        QDropEvent drop2(ins, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(viewer, &drop2);
        QCOMPARE(layout(app.tl(), kV1), QStringLiteral("0:100 100:250 350:150"));

        // Between the tiles the drop is refused and nothing changes.
        QDragEnterEvent enter3(DragPos(overlay->mapTo(viewer, QPoint(1, 1))), Qt::CopyAction,
            &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(viewer, &enter3);
        QDragMoveEvent move3(DragPos(overlay->mapTo(viewer, QPoint(1, 1))), Qt::CopyAction, &mime,
            Qt::LeftButton, {});
        QCoreApplication::sendEvent(viewer, &move3);
        QVERIFY(!move3.isAccepted());
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(viewer, &leave);
        QVERIFY(!overlay->isVisible());
    }

    /// Typing a timecode with the timeline focused shows it in the viewer and moves the
    /// playhead on Enter (spec 2.10); the number keys do not reach their shortcuts.
    void spec2_10_typedTimecodeInTheWindow()
    {
        App app;
        app.timeline()->setFocus();
        QTRY_VERIFY(app.timeline()->hasFocus());
        QTest::keyClick(app.timeline(), Qt::Key_2);
        QTest::keyClick(app.timeline(), Qt::Key_0);
        QTest::keyClick(app.timeline(), Qt::Key_0);
        auto* label = app.viewer()->findChild<QLabel*>(QStringLiteral("TimecodeLabel"));
        QVERIFY(label);
        QCOMPARE(label->text(), QStringLiteral("00:00:02:00"));
        shot(app.viewer(), "timecode_entry");
        QTest::keyClick(app.timeline(), Qt::Key_Return);
        QCOMPARE(app.project.timeline()->playhead(), int64_t(50));
        QCOMPARE(label->text(), QStringLiteral("00:00:02:00")); // the position now
        QTest::keyClick(app.timeline(), Qt::Key_Minus);
        QTest::keyClick(app.timeline(), Qt::Key_1);
        QTest::keyClick(app.timeline(), Qt::Key_0);
        QTest::keyClick(app.timeline(), Qt::Key_Enter);
        QCOMPARE(app.project.timeline()->playhead(), int64_t(40));
    }

    /// Rolling a cut with real media shows both frames in the timeline viewer and the offset
    /// next to the pointer; the view closes on release (spec 8.15, 21.9).
    void spec8_15_twoUpWhileRolling()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        App app;
        const ImportResult imported = app.project.importMedia({m_media});
        const QString id = imported.importedIds.front();
        TimelineController* c = app.page()->timeline()->controller();
        c->insertSource(id, 0, 4'000'000, SourceEditMode::Overwrite); // 0..100
        c->insertSource(id, 5'000'000, 9'000'000, SourceEditMode::Overwrite); // 100..200
        QCOMPARE(layout(app.tl(), kV1), QStringLiteral("0:100 100:100"));
        TimelineWidget* w = app.timeline();
        w->setPixelsPerFrame(5.0, timelinemetrics::kHeaderWidth);
        w->scrollToFrame(0);
        c->setTool(TimelineTool::Trim);
        const TimelineLayout& L = w->layout();
        const int y = L.rowRect(L.rowOf(kV1)).center().y();
        const QPoint cut(int(std::lround(L.frameToX(100))) + 1, y);
        QTest::mousePress(w->viewport(), Qt::LeftButton, {}, cut);
        const QPoint to = cut + QPoint(int(std::lround(12 * L.pixelsPerFrame())), 0);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(w->viewport(), cut + (to - cut) * i / 4);
        TrimTwoUpView* twoUp = app.viewer()->trimView();
        QVERIFY(twoUp && twoUp->isVisible());
        QVERIFY(!w->dragInfo().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(twoUp->hasPicture(0) && twoUp->hasPicture(1), 10000);
        shot(app.window.get(), "two_up_trim");
        shot(w, "drag_label_roll");
        QTest::mouseRelease(w->viewport(), Qt::LeftButton, {}, to);
        QVERIFY(!twoUp->isVisible());
        QVERIFY(w->dragInfo().isEmpty());

        // A move shows the offset label too (screenshot only).
        c->setTool(TimelineTool::Select);
        const QPoint body(int(std::lround(L.frameToX(150))), y);
        QTest::mousePress(w->viewport(), Qt::LeftButton, {}, body);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(w->viewport(), body + QPoint(15 * i, 0));
        QVERIFY(!w->dragInfo().isEmpty());
        shot(w, "drag_label_move");
        QTest::keyClick(w, Qt::Key_Escape); // cancel: nothing changes
        QTest::mouseRelease(w->viewport(), Qt::LeftButton, {}, body + QPoint(60, 0));
    }
};

QTEST_MAIN(TestEditPolish)
#include "test_editpolish.moc"
