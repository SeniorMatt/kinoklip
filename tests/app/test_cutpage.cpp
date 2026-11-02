// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Cut page (docs/behavior-spec.md section 35): page wiring and shared
// state with the Edit page, smart edits through the page (one undo step each, nearest edit
// point), the source tape across clip boundaries, the dual timeline staying in sync, ripple
// edge trims in the lower timeline, the strip view and fast review speeds.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/CutPage.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/SourcePlayer.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/cut/CutEditor.h"
#include "ui/cut/CutOverview.h"
#include "ui/cut/CutToolBar.h"
#include "ui/cut/CutViewer.h"
#include "ui/cut/FastReview.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace kk;

namespace {

constexpr int64_t kSecondUs = 1'000'000;

/// Everything main() creates, for one test, with three fake 2 s / 3 s / 4 s clips.
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QStringList ids;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        int seconds = 2;
        for (const char* name : {"alpha", "bravo", "charlie"}) {
            MediaItem m;
            m.name = QLatin1String(name);
            m.path = QStringLiteral("/nonexistent/%1.mp4").arg(m.name);
            m.info.valid = true;
            m.info.hasVideo = true;
            m.info.hasAudio = true;
            m.info.durationUs = seconds++ * kSecondUs;
            m.info.frameRate = Rational {25, 1};
            m.info.width = 1920;
            m.info.height = 1080;
            ids.append(project.mediaPool()->addItem(m));
        }
        MainWindowOptions options;
        options.autosave = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    CutPage* cut() const { return window->cutPage(); }
    const TimelineData& data() const { return project.timeline()->data(); }

    /// Shows the window on the Cut page.
    void showCut()
    {
        window->resize(1600, 1000);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action("view.page_cut")->trigger();
        QCoreApplication::processEvents();
    }

    /// Whole clips of items @p indices.
    QVector<SourceSelection> whole(std::initializer_list<int> indices) const
    {
        QVector<SourceSelection> out;
        for (int i : indices)
            out.append(SourceSelection {ids[i], 0, 0});
        return out;
    }
};

QString layout(const TimelineData& d, TrackType type, int index)
{
    QStringList parts;
    for (const ClipData& c : d.tracks(type)[size_t(index)].clips)
        parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
    return parts.join(QLatin1Char(' '));
}

} // namespace

class TestCutPage : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_cutpage"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // ---- 35.1 / 35.2 page wiring ----
    void pageIsEnabledAndSwitchesWithShift3()
    {
        Fixture f;
        QVERIFY(f.cut());
        QVERIFY(f.action("view.page_cut")->isEnabled());
        QCOMPARE(f.actions.shortcuts(QStringLiteral("view.page_cut")).value(0),
            QKeySequence(QStringLiteral("Shift+3")));
        f.action("view.page_cut")->trigger();
        QCOMPARE(f.window->currentPageId(), QStringLiteral("cut"));
        QVERIFY(f.action("view.page_cut")->isChecked());
        // The interface bar offers the media pool toggle there, not the inspector.
        QVERIFY(f.action("view.toggle_media_pool")->isEnabled());
        QVERIFY(!f.action("view.toggle_inspector")->isEnabled());
        f.action("view.toggle_media_pool")->trigger();
        QVERIFY(!f.cut()->isPanelVisible(PanelId::MediaPool));
    }

    void cutActionsOnlyOnTheCutPage()
    {
        Fixture f;
        f.showCut();
        QVERIFY(f.action("cut.smart_insert")->isEnabled());
        QVERIFY(f.action("cut.fast_review")->isEnabled());
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!f.action("cut.smart_insert")->isEnabled());
        QVERIFY(!f.action("cut.viewer_source_tape")->isEnabled());
    }

    void sharesTimelinePlayheadAndSelectionWithEditPage()
    {
        Fixture f;
        f.showCut();
        QVERIFY(f.cut()->editor()->run(CutEdit::Append, f.whole({0, 1})));
        const quint64 first = f.data().videoTracks[0].clips[0].id;
        f.selection.selectClip(first);
        f.project.timeline()->setPlayhead(30);
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(f.project.timeline()->playhead(), int64_t(30));
        QVERIFY(f.selection.isSelected(first));
        // Both pages edit with one controller: the Edit page's timeline sees the same clips.
        QCOMPARE(f.window->editPage()->timeline()->controller(), f.cut()->timeline()->controller());
        f.action("view.page_cut")->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(f.project.timeline()->playhead(), int64_t(30));
        QVERIFY(f.selection.isSelected(first));
    }

    // ---- 35.6 - 35.10 smart edits, one undo step each ----
    void smartInsertPicksNearestEditPointInOneUndoStep()
    {
        Fixture f;
        f.showCut();
        CutEditor* editor = f.cut()->editor();
        QVERIFY(editor->run(CutEdit::Append, f.whole({0, 1}))); // 0:50 50:75
        QCOMPARE(layout(f.data(), TrackType::Video, 0), QStringLiteral("0:50 50:75"));
        const int steps = f.project.undoStack()->count();
        f.project.timeline()->setPlayhead(60); // nearest edit point: 50
        QVERIFY(editor->run(CutEdit::SmartInsert, f.whole({2})));
        QCOMPARE(layout(f.data(), TrackType::Video, 0), QStringLiteral("0:50 50:100 150:75"));
        QCOMPARE(layout(f.data(), TrackType::Audio, 0), QStringLiteral("0:50 50:100 150:75"));
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Smart Insert"));
        QCOMPARE(f.project.timeline()->playhead(), int64_t(150)); // after the new clip
        QCOMPARE(f.selection.clips().size(), 2); // video + audio of the new clip
        f.project.undo();
        QCOMPARE(layout(f.data(), TrackType::Video, 0), QStringLiteral("0:50 50:75"));
    }

    void appendPlaceOnTopCloseUpRippleOverwrite()
    {
        Fixture f;
        f.showCut();
        CutEditor* editor = f.cut()->editor();
        QUndoStack* undo = f.project.undoStack();
        int steps = undo->count();
        QVERIFY(editor->run(CutEdit::Append, f.whole({2})));
        QVERIFY(editor->run(CutEdit::Append, f.whole({0})));
        QCOMPARE(undo->count(), steps + 2);
        QCOMPARE(layout(f.data(), TrackType::Video, 0), QStringLiteral("0:100 100:50"));

        f.project.timeline()->setPlayhead(20);
        steps = undo->count();
        QVERIFY(editor->run(CutEdit::PlaceOnTop, f.whole({0})));
        QCOMPARE(undo->count(), steps + 1);
        QCOMPARE(layout(f.data(), TrackType::Video, 1), QStringLiteral("20:50"));

        f.project.timeline()->setPlayhead(80); // V2 is busy until 70: close up on V2 at 80
        QVERIFY(editor->run(CutEdit::CloseUp, f.whole({1})));
        QCOMPARE(undo->count(), steps + 2);
        QCOMPARE(layout(f.data(), TrackType::Video, 1), QStringLiteral("20:50 80:75"));
        QCOMPARE(undo->undoText(), QStringLiteral("Close Up"));

        f.project.timeline()->setPlayhead(120); // over the 2 s clip on V1 [100, 150)
        QVERIFY(editor->run(CutEdit::RippleOverwrite, f.whole({1}))); // 3 s replaces it
        QCOMPARE(layout(f.data(), TrackType::Video, 0), QStringLiteral("0:100 100:75"));
        QCOMPARE(undo->count(), steps + 3);
    }

    void refusedEditLeavesNoUndoStep()
    {
        Fixture f;
        f.showCut();
        QSignalSpy refused(f.cut()->editor(), &CutEditor::editRefused);
        const int steps = f.project.undoStack()->count();
        QVERIFY(!f.cut()->editor()->run(CutEdit::RippleOverwrite, f.whole({0}))); // empty timeline
        QVERIFY(!f.cut()->editor()->run(CutEdit::SourceOverwrite, f.whole({0}))); // no timecode
        QVERIFY(!f.cut()->editor()->run(CutEdit::Append, {})); // no material
        QCOMPARE(refused.count(), 3);
        QCOMPARE(f.project.undoStack()->count(), steps);
    }

    void insertKeyRunsSmartInsertOnTheCutPage()
    {
        Fixture f;
        f.showCut();
        QVERIFY(f.cut()->editor()->run(CutEdit::Append, f.whole({0, 1})));
        f.cut()->viewer()->clipViewer()->setSourceMedia(f.ids[2]);
        f.cut()->viewer()->setMode(CutViewer::Mode::SourceClip);
        f.project.timeline()->setPlayhead(45); // nearest: 50
        f.action("edit.insert")->trigger(); // F9
        QCOMPARE(layout(f.data(), TrackType::Video, 0), QStringLiteral("0:50 50:100 150:75"));
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Smart Insert"));
        // On the Edit page F9 is the normal insert at the playhead again.
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        f.project.timeline()->setPlayhead(10);
        const int steps = f.project.undoStack()->count();
        f.action("edit.insert")->trigger(); // the Edit page's source viewer is empty
        QCOMPARE(f.project.undoStack()->count(), steps);
        QCOMPARE(layout(f.data(), TrackType::Video, 0), QStringLiteral("0:50 50:100 150:75"));
    }

    // ---- 35.4 / 35.5 source tape ----
    void sourceTapeMarksAcrossClipBoundaries()
    {
        Fixture f;
        f.showCut();
        CutViewer* viewer = f.cut()->viewer();
        f.action("cut.viewer_source_tape")->trigger();
        QCOMPARE(viewer->mode(), CutViewer::Mode::SourceTape);
        const SourceTape& tape = viewer->tape();
        QCOMPARE(int(tape.entries().size()), 3); // the bin's three clips, 50 + 75 + 100 frames
        QCOMPARE(tape.duration(), int64_t(225));
        SourcePlayer* player = viewer->tapeViewer()->sourcePlayer();
        QVERIFY(player);
        // In inside alpha, out inside charlie: three ranges.
        player->setMarks(40, 135);
        const QVector<SourceSelection> material = viewer->sourceMaterial();
        QCOMPARE(material.size(), 3);
        QCOMPARE(material[0].mediaId, f.ids[0]);
        QCOMPARE(material[0].inUs, int64_t(1'600'000));
        QCOMPARE(material[0].outUs, int64_t(2'000'000));
        QCOMPARE(material[1].mediaId, f.ids[1]);
        QCOMPARE(material[1].outUs, int64_t(3'000'000));
        QCOMPARE(material[2].mediaId, f.ids[2]);
        QCOMPARE(material[2].outUs, int64_t(400'000));
        // The buttons edit from the tape: one Append, three clips back to back.
        const int steps = f.project.undoStack()->count();
        f.action("cut.append")->trigger();
        QCOMPARE(layout(f.data(), TrackType::Video, 0), QStringLiteral("0:10 10:75 85:10"));
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        // No marks: the whole clip under the tape position.
        player->setMarks(-1, -1);
        player->engine()->seek(60);
        QCOMPARE(viewer->sourceMaterial().size(), 1);
        QCOMPARE(viewer->sourceMaterial().front().mediaId, f.ids[1]);
        // A click on a media pool clip moves the tape to it.
        QVERIFY(viewer->showTapeClip(f.ids[2]));
        QCOMPARE(player->engine()->position(), int64_t(125));
        QCOMPARE(viewer->tapeClipAtPosition(), f.ids[2]);
    }

    void viewerModesAndQ()
    {
        Fixture f;
        f.showCut();
        CutViewer* viewer = f.cut()->viewer();
        f.action("cut.viewer_source_clip")->trigger();
        QCOMPARE(viewer->mode(), CutViewer::Mode::SourceClip);
        QVERIFY(viewer->clipViewer()->isVisible());
        QVERIFY(!viewer->timelineViewer()->isVisible());
        f.action("view.toggle_source_timeline")->trigger(); // Q
        QCOMPARE(viewer->mode(), CutViewer::Mode::Timeline);
        QVERIFY(viewer->timelineViewer()->isVisible());
        QCOMPARE(ViewerPanel::activeViewer(), viewer->timelineViewer());
        f.action("view.toggle_source_timeline")->trigger();
        QCOMPARE(viewer->mode(), CutViewer::Mode::SourceClip);
        QVERIFY(f.action("cut.viewer_source_clip")->isChecked());
    }

    // ---- 35.3 dual timeline ----
    void dualTimelineFollowsThePlayhead()
    {
        Fixture f;
        f.showCut();
        QVERIFY(f.cut()->editor()->run(CutEdit::Append, f.whole({2, 2, 2, 2, 2, 2})));
        TimelineWidget* lower = f.cut()->timeline();
        CutOverview* overview = f.cut()->overview();
        QVERIFY(lower->viewOptions().centredPlayhead);
        const double fps = 25.0;
        // Default zoom: about 20 s across the lower timeline.
        const auto [a, b] = lower->visibleFrameRange();
        QVERIFY2(std::abs((b - a) - CutPage::kLowerTimelineSeconds * fps) < 5.0,
            qPrintable(QStringLiteral("%1 frames").arg(b - a)));
        for (const int64_t frame : {400, 520, 300}) {
            f.project.timeline()->setPlayhead(frame);
            const auto [first, last] = lower->visibleFrameRange();
            QVERIFY2(std::abs((first + last) / 2.0 - double(frame)) < 2.0,
                qPrintable(QStringLiteral("%1 .. %2 for %3").arg(first).arg(last).arg(frame)));
            QCOMPARE(overview->detailFirst(), first);
            QCOMPARE(overview->detailLast(), last);
        }
        // Clicking the overview moves the playhead, the lower timeline follows.
        const QPoint at(int(overview->frameToX(350.0)) + 1, overview->height() / 2);
        QTest::mouseClick(overview, Qt::LeftButton, {}, at);
        QVERIFY(std::abs(f.project.timeline()->playhead() - 350) <= 2);
        const auto [first, last] = lower->visibleFrameRange();
        QVERIFY(std::abs((first + last) / 2.0 - double(f.project.timeline()->playhead())) < 2.0);
        QCOMPARE(f.cut()->viewer()->mode(), CutViewer::Mode::Timeline);
    }

    // ---- 35.12 lower timeline editing ----
    void edgeDragRipplesInTheLowerTimeline()
    {
        Fixture f;
        f.showCut();
        QVERIFY(f.cut()->editor()->run(CutEdit::Append, f.whole({0, 1}))); // 0:50 50:75
        f.selection.clear();
        TimelineWidget* lower = f.cut()->timeline();
        f.project.timeline()->setPlayhead(40);
        QCoreApplication::processEvents();
        const TimelineLayout& geo = lower->layout();
        const int row = geo.rowOf(TrackRef {TrackType::Video, 0});
        QVERIFY(row >= 0);
        const int y = geo.rowRect(row).center().y();
        // The tail of alpha, a little inside the clip (not the roll zone at the cut).
        const int x = int(std::lround(geo.frameToX(50.0))) - 4;
        const int dx = int(std::lround(10 * geo.pixelsPerFrame()));
        QWidget* vp = lower->viewport();
        const int steps = f.project.undoStack()->count();
        QTest::mousePress(vp, Qt::LeftButton, {}, QPoint(x, y));
        QTest::mouseMove(vp, QPoint(x - dx / 2, y));
        QTest::mouseMove(vp, QPoint(x - dx, y));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, QPoint(x - dx, y));
        const QString v1 = layout(f.data(), TrackType::Video, 0);
        // Ripple: bravo moved left with the tail, no gap.
        const ClipData& alpha = f.data().videoTracks[0].clips[0];
        const ClipData& bravo = f.data().videoTracks[0].clips[1];
        QVERIFY2(alpha.duration < 50, qPrintable(v1));
        QCOMPARE(bravo.start, alpha.end());
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Ripple Trim"));
    }

    void movePastTheTopTrackCreatesATrack()
    {
        Fixture f;
        f.showCut();
        QVERIFY(f.cut()->editor()->run(CutEdit::Append, f.whole({0, 1}))); // V1 + A1
        f.selection.clear();
        TimelineWidget* lower = f.cut()->timeline();
        f.project.timeline()->setPlayhead(40);
        QCoreApplication::processEvents();
        const TimelineLayout& geo = lower->layout();
        const int videoTracks = int(f.data().videoTracks.size());
        const int v1 = geo.rowOf(TrackRef {TrackType::Video, 0});
        const int top = geo.rowOf(TrackRef {TrackType::Video, videoTracks - 1});
        QVERIFY(v1 >= 0 && top >= 0);
        const QPoint from(int(std::lround(geo.frameToX(20.0))), geo.rowRect(v1).center().y());
        // Above the top video track: the compact lower timeline has no empty band there, so the
        // pointer goes onto the ruler, which counts as "past the outermost track" too.
        const int aboveY = std::max(1, geo.rowRect(top).top() - 6);
        const QPoint to(from.x(), aboveY);
        QWidget* vp = lower->viewport();
        QTest::mousePress(vp, Qt::LeftButton, {}, from);
        QTest::mouseMove(vp, QPoint(from.x(), (from.y() + to.y()) / 2));
        QTest::mouseMove(vp, to);
        QTest::mouseRelease(vp, Qt::LeftButton, {}, to);
        // Spec 21.8 on the Cut page too: one new video track (and its audio partner).
        QCOMPARE(int(f.data().videoTracks.size()), videoTracks + 1);
        QCOMPARE(int(f.data().videoTracks.back().clips.size()), 1);
        QCOMPARE(f.data().videoTracks.back().clips.front().start, int64_t(0));
    }

    void bladeAndDissolveFromTheToolBar()
    {
        Fixture f;
        f.showCut();
        QVERIFY(f.cut()->editor()->run(CutEdit::Append, f.whole({2})));
        f.selection.clear();
        f.project.timeline()->setPlayhead(30);
        f.action("edit.split")->trigger(); // Ctrl+\ (keymap)
        QCOMPARE(layout(f.data(), TrackType::Video, 0), QStringLiteral("0:30 30:70"));
        auto* dissolve
            = f.cut()->toolBar()->findChild<QToolButton*>(QStringLiteral("CutDissolveButton"));
        QVERIFY(dissolve);
        dissolve->click();
        QVERIFY(f.data().videoTracks[0].clips[0].transitionOut.has_value()
            || f.data().videoTracks[0].clips[1].transitionIn.has_value());
    }

    // ---- 35.13 media pool views ----
    void stripViewOnlyOnTheCutPage()
    {
        Fixture f;
        f.showCut();
        MediaPoolPanel* cutPool = f.cut()->mediaPool();
        QVERIFY(cutPool->isStripViewAvailable());
        QVERIFY(!f.window->editPage()->mediaPool()->isStripViewAvailable());
        cutPool->setViewMode(MediaPoolPanel::ViewMode::Strip);
        QCOMPARE(cutPool->viewMode(), MediaPoolPanel::ViewMode::Strip);
        auto* strip = cutPool->findChild<QWidget*>(QStringLiteral("MediaPoolStrip"));
        QVERIFY(strip && strip->isVisible());
        cutPool->setViewMode(MediaPoolPanel::ViewMode::List);
        QVERIFY(!strip->isVisible());
    }

    // ---- 35.14 fast review ----
    void fastReviewSpeedGrowsWithShotLength()
    {
        Fixture f;
        QVERIFY(f.cut()->editor()->run(CutEdit::Append, f.whole({0, 2})));
        f.cut()->editor()->run(CutEdit::Append, {SourceSelection {f.ids[2], 0, 10 * kSecondUs}});
        FastReview* review = f.cut()->fastReview();
        QCOMPARE(review->rateAt(10), 1.0); // 2 s shot
        QCOMPARE(review->rateAt(60), 2.0); // 4 s shot
        QVERIFY(review->rateAt(10) < review->rateAt(60));
        review->start(); // no program engine in this test: nothing happens
        QVERIFY(!review->isActive());
    }
};

QTEST_MAIN(TestCutPage)
#include "test_cutpage.moc"
