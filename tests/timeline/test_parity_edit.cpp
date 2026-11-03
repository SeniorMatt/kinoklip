// SPDX-License-Identifier: GPL-3.0-or-later
// Behaviour parity tests, part 2 (resolveedit branch): ripple cut / ripple delete on clips,
// gaps and in-out ranges, ripple overwrite, selection commands, join, swap, match frame,
// track targeting keys, clip duration / speed. Test names start with the item number of
// docs/behavior-spec.md.
#include "ParityTestUtils.h"
#include "TimelineTestUtils.h"
#include "core/EditBehaviour.h"
#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"
#include "render/TimelineEval.h"
#include "ui/common/SettingsPages.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QAction>
#include <QComboBox>
#include <QScrollBar>
#include <QTest>
#include <QUndoStack>
#include <QWheelEvent>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;

namespace {
constexpr TrackRef kA2 {TrackType::Audio, 1};
} // namespace

class TestParityEdit : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_parity_edit"); }
    void init() { QSettings().clear(); }

    // ---- 10 Delete, ripple delete, ripple cut ----

    void spec10_2_rippleDeleteClipRipplesAutoSelectTracks()
    {
        Ctl t;
        const quint64 v = t.put(kV1, solidClip(10, 10));
        const quint64 a = t.put(kA1, solidClip(10, 10));
        t.link({v, a});
        t.put(kV1, solidClip(20, 10));
        t.put(kA1, solidClip(20, 10));
        t.put(kV2, solidClip(40, 5)); // title later on V2: follows (spec 10.4)
        t.f.selection.setClips({v});
        const int before = t.undoCount();
        t.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("10:10")); // linked part went too
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("30:5"));
        QCOMPARE(t.undoCount(), before + 1); // one undo step
        QVERIFY(t.f.selection.clips().isEmpty());
        t.f.project.undoStack()->undo();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:10 20:10"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("40:5"));
    }

    void spec10_3_rippleDeleteLinkedSelectionOffAndLockedTracks()
    {
        Ctl t;
        const quint64 v = t.put(kV1, solidClip(0, 10));
        const quint64 a = t.put(kA1, solidClip(0, 10));
        t.link({v, a});
        t.put(kV1, solidClip(10, 10));
        t.put(kA1, solidClip(10, 10));
        t.put(kA2, solidClip(30, 10));
        t.f.project.beginEdit(QStringLiteral("lock"));
        t.f.data().audioTracks[1].locked = true;
        t.f.project.endEdit();
        t.c.setLinkedSelection(false);
        t.f.selection.setClips({v});
        t.action("edit.ripple_delete")->trigger();
        // Only the video part; A1 has a clip in the range, so it keeps its place; A2 is locked.
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(t.f.view(), kA2), QStringLiteral("30:10"));
    }

    void spec10_5_rippleDeleteGapStillWorks()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 10));
        t.put(kV1, solidClip(20, 10));
        QVERIFY(t.c.selectGapAt(kV1, 15));
        t.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10"));
    }

    void spec10_7_backspaceLiftsMarkedRange()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 40));
        t.put(kA1, solidClip(0, 40));
        t.put(kV2, solidClip(0, 40));
        t.f.project.beginEdit(QStringLiteral("auto select off"));
        t.f.data().videoTracks[1].autoSelect = false;
        t.f.project.endEdit();
        t.setMarks(10, 20);
        t.action("edit.delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 20:20"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("0:10 20:20"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:40")); // auto select off
        QCOMPARE(t.f.view().markIn, int64_t(10)); // lift keeps the marks
    }

    void spec10_8_deleteExtractsMarkedRange()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 40));
        t.put(kA1, solidClip(30, 10));
        t.setMarks(10, 20);
        t.tl()->setPlayhead(35);
        const int before = t.undoCount();
        t.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:20"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("20:10"));
        QCOMPARE(t.f.view().markIn, int64_t(-1));
        QCOMPARE(t.tl()->playhead(), int64_t(10));
        QCOMPARE(t.undoCount(), before + 1);
        // A selection wins over the marks.
        t.setMarks(0, 5);
        const quint64 last = t.f.view().videoTracks[0].clips.back().id;
        t.f.selection.setClips({last});
        t.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10"));
    }

    void spec10_9_rippleCutClipsAndPasteBack()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 10, QStringLiteral("a")));
        const quint64 b = t.put(kV1, solidClip(10, 10, QStringLiteral("b")));
        t.put(kV1, solidClip(20, 10, QStringLiteral("c")));
        t.f.selection.setClips({b});
        QCOMPARE(
            t.action("edit.ripple_cut")->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+X")));
        const int before = t.undoCount();
        t.action("edit.ripple_cut")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(t.undoCount(), before + 1);
        QCOMPARE(t.c.clipboard().clips.size(), 1);
        // Paste insert at the cut puts it back where it was.
        t.tl()->setPlayhead(10);
        t.action("edit.paste_insert")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10 20:10"));
        QCOMPARE(t.f.view().videoTracks[0].clips[1].name, QStringLiteral("b"));
    }

    void spec10_9_rippleCutMarkedRange()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 40));
        t.setMarks(10, 20);
        t.action("edit.ripple_cut")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:20"));
        QCOMPARE(t.c.clipboard().duration, int64_t(10));
        QCOMPARE(t.tl()->playhead(), int64_t(10));
        // Nothing selected, no marks: nothing happens and no undo step.
        const int before = t.undoCount();
        t.action("edit.ripple_cut")->trigger();
        QCOMPARE(t.undoCount(), before);
    }

    void spec10_10_deleteGaps()
    {
        Ctl t;
        t.put(kV1, solidClip(5, 10));
        t.put(kV1, solidClip(30, 10));
        t.put(kA1, solidClip(32, 4));
        t.action("edit.delete_gaps")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("12:4")); // stays in sync with V1
    }

    void spec16_5_copyAndCutMarkedRange()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 40));
        t.setMarks(10, 20);
        t.action("edit.copy")->trigger();
        QCOMPARE(t.c.clipboard().duration, int64_t(10));
        t.action("edit.cut")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 20:20"));
    }

    // ---- 4 Source edits ----

    void spec4_11_rippleOverwriteReplacesClipAtItsOwnLength()
    {
        Ctl t;
        const QString media = addMedia(t.f.project, true, false);
        t.put(kV1, solidClip(0, 10));
        t.put(kV1, solidClip(10, 20));
        t.put(kV1, solidClip(30, 10));
        t.tl()->setPlayhead(15);
        // Source: 1 s (25 frames) replaces the 20-frame clip, the rest moves 5 frames later.
        const QVector<quint64> ids
            = t.c.insertSource(media, 0, 1'000'000, SourceEditMode::RippleOverwrite);
        QCOMPARE(ids.size(), 1);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:25 35:10"));
        QCOMPARE(t.tl()->playhead(), int64_t(35));
        // With in/out marks the marked range is replaced.
        t.setMarks(0, 10);
        t.c.insertSource(media, 0, 200'000, SourceEditMode::RippleOverwrite); // 5 frames
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:5 5:25 30:10"));
    }

    // ---- 7 Selection ----

    void spec7_11_selectClipsForwardAndBackward()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(0, 10));
        const quint64 b = t.put(kV1, solidClip(20, 10));
        const quint64 c = t.put(kA1, solidClip(25, 10));
        const quint64 v2 = t.put(kV2, solidClip(30, 5));
        t.f.project.beginEdit(QStringLiteral("auto select off"));
        t.f.data().videoTracks[1].autoSelect = false;
        t.f.project.endEdit();
        t.tl()->setPlayhead(15);
        t.action("edit.select_forward")->trigger();
        QCOMPARE(t.f.selection.clips(), (QVector<quint64> {b, c}));
        t.action("edit.select_forward_all")->trigger();
        QCOMPARE(t.f.selection.clips(), (QVector<quint64> {b, v2, c}));
        t.action("edit.select_backward")->trigger();
        QCOMPARE(t.f.selection.clips(), (QVector<quint64> {a}));
    }

    void spec7_12_selectNearestEditPoint()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(0, 10));
        const quint64 b = t.put(kV1, solidClip(10, 10));
        t.tl()->setPlayhead(12);
        t.action("trim.select_nearest_edit")->trigger(); // V
        QCOMPARE(t.c.selectedEdit(), (EditPointSelection {a, b})); // a cut: roll
        t.tl()->setPlayhead(19);
        t.action("trim.select_nearest_edit")->trigger();
        QCOMPARE(t.c.selectedEdit(), (EditPointSelection {b, 0})); // free tail
    }

    void spec7_13_selectNearestClipOrGap()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(0, 10));
        t.put(kV1, solidClip(20, 10));
        t.tl()->setPlayhead(5);
        t.action("trim.select_nearest_clip")->trigger(); // Shift+V
        QCOMPARE(t.f.selection.clips(), (QVector<quint64> {a}));
        t.tl()->setPlayhead(15);
        t.action("trim.select_nearest_clip")->trigger();
        QVERIFY(t.c.selectedGap().has_value());
        QCOMPARE(t.c.selectedGap()->start, int64_t(10));
        QVERIFY(t.f.selection.clips().isEmpty());
    }

    // ---- 9 Blade / join ----

    void spec9_6_joinThroughEdit()
    {
        Ctl t;
        const quint64 v = t.put(kV1, mediaClip(0, 40));
        const quint64 a = t.put(kA1, mediaClip(0, 40));
        t.link({v, a});
        t.tl()->setPlayhead(20);
        t.action("edit.split")->trigger(); // Ctrl+\ on the auto-select tracks
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:20 20:20"));
        const quint64 right = t.f.view().videoTracks[0].clips[1].id;
        t.c.setSelectedEdit({v, right});
        const int before = t.undoCount();
        t.action("edit.join")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:40"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("0:40")); // the linked audio too
        QCOMPARE(t.undoCount(), before + 1);
    }

    // ---- 18 Markers ----

    void spec18_6_deleteMarkerAtPlayhead()
    {
        Ctl t;
        t.tl()->setPlayhead(7);
        t.action("mark.add_marker")->trigger();
        QCOMPARE(t.f.view().markers.size(), 1);
        t.action("mark.delete_marker")->trigger(); // Alt+M
        QCOMPARE(t.f.view().markers.size(), 0);
    }

    // ---- 21 Moving clips ----

    void spec21_7_swapKeysExchangeNeighbours()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(0, 10, QStringLiteral("a")));
        const quint64 aa = t.put(kA1, solidClip(0, 10, QStringLiteral("a")));
        t.link({a, aa});
        const quint64 b = t.put(kV1, solidClip(10, 20, QStringLiteral("b")));
        const quint64 ba = t.put(kA1, solidClip(10, 20, QStringLiteral("b")));
        t.link({b, ba});
        t.f.selection.setClips({a});
        t.action("trim.swap_right")->trigger(); // Ctrl+Shift+.
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:20 20:10"));
        QCOMPARE(t.f.view().videoTracks[0].clips[0].id, b);
        QCOMPARE(t.f.view().audioTracks[0].clips[0].id, ba); // linked parts swapped along
        t.action("trim.swap_left")->trigger();
        QCOMPARE(t.f.view().videoTracks[0].clips[0].id, a);
    }

    // ---- 19 Zoom and scrolling ----

    void spec19_6_wheelZoomAroundPlayheadOrCursor()
    {
        View v;
        v.put(kV1, solidClip(0, 5000));
        v.w.setPixelsPerFrame(2.0, kHeaderWidth);
        v.tl()->setPlayhead(100);
        auto altWheel = [&](QPoint pos) {
            QWheelEvent wheel(pos, v.vp()->mapToGlobal(pos), {}, QPoint(0, 240), Qt::NoButton,
                Qt::AltModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(v.vp(), &wheel);
        };
        // Default (Resolve): Alt+wheel zooms and the playhead keeps its place.
        const double playheadX = v.w.layout().frameToX(100);
        const QPoint pointer(900, 200);
        altWheel(pointer);
        QVERIFY(v.w.pixelsPerFrame() > 2.0);
        QVERIFY(qAbs(v.w.layout().frameToX(100) - playheadX) < 1.0);
        // "Zoom around: Mouse cursor": the frame under the pointer keeps its place.
        EditBehaviour::setZoomAnchor(ZoomAnchor::Cursor);
        const double frameUnderPointer = v.w.layout().xToFrame(pointer.x());
        altWheel(pointer);
        QVERIFY(qAbs(v.w.layout().xToFrame(pointer.x()) - frameUnderPointer) < 1.0);
        // Keyboard zoom always keeps the playhead.
        const double before = v.w.layout().frameToX(100);
        v.action("timeline.zoom_in")->trigger();
        QVERIFY(qAbs(v.w.layout().frameToX(100) - before) < 1.0);
    }

    void spec19_6_editingPreferencesPage()
    {
        // The Editing page (src/ui/dialogs) writes the keys EditBehaviour reads.
        SettingsPageRegistry registry;
        registerStandardSettingsPages(registry, nullptr);
        QVERIFY(registry.contains(QStringLiteral("editing")));
        QWidget parent;
        SettingsPage* page = nullptr;
        for (const SettingsPageInfo& info : registry.pages()) {
            if (info.id == QLatin1String("editing"))
                page = info.create(&parent);
        }
        QVERIFY(page);
        auto* anchor = page->findChild<QComboBox*>(QStringLiteral("ZoomAnchor"));
        auto* scheme = page->findChild<QComboBox*>(QStringLiteral("WheelScheme"));
        QVERIFY(anchor && scheme);
        QCOMPARE(EditBehaviour::fromSettings().zoomAnchor, ZoomAnchor::Playhead); // default
        anchor->setCurrentIndex(anchor->findData(QStringLiteral("mouse")));
        scheme->setCurrentIndex(scheme->findData(int(WheelScheme::Classic)));
        page->apply();
        QCOMPARE(EditBehaviour::fromSettings().zoomAnchor, ZoomAnchor::Cursor);
        QCOMPARE(EditBehaviour::fromSettings().wheelScheme, WheelScheme::Classic);
    }

    void spec19_3_resolveWheelScrollsAndChangesTrackHeight()
    {
        View v;
        v.put(kV1, solidClip(0, 5000));
        v.w.setPixelsPerFrame(2.0, kHeaderWidth);
        const double ppf = v.w.pixelsPerFrame();
        auto wheel = [&](Qt::KeyboardModifiers mods, int dy) {
            const QPoint pos(600, 200);
            QWheelEvent e(pos, v.vp()->mapToGlobal(pos), {}, QPoint(0, dy), Qt::NoButton, mods,
                Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(v.vp(), &e);
        };
        // Ctrl+wheel scrolls time (no zoom).
        const int scrollBefore = v.w.horizontalScrollBar()->value();
        wheel(Qt::ControlModifier, -240);
        QVERIFY(v.w.horizontalScrollBar()->value() > scrollBefore);
        QCOMPARE(v.w.pixelsPerFrame(), ppf);
        // Shift+wheel makes every track taller (view only, no undo step).
        const int undo = v.undoCount();
        const int rowHeight = v.w.layout().rowRect(v.w.layout().rowOf(kV1)).height();
        wheel(Qt::ShiftModifier, 240);
        QVERIFY(v.w.trackHeightScale() > 1.0);
        QVERIFY(v.w.layout().rowRect(v.w.layout().rowOf(kV1)).height() > rowHeight);
        QCOMPARE(v.undoCount(), undo);
        QCOMPARE(v.f.view().videoTracks[0].height, 0);
    }

    // ---- 8 Dynamic trim ----

    void spec8_17_dynamicTrimFollowsPlayback()
    {
        Ctl t;
        bool playing = false;
        t.c.setPlayingProvider([&] { return playing; });
        const quint64 a = t.put(kV1, mediaClip(0, 20, 0));
        const quint64 b = t.put(kV1, mediaClip(20, 20, 50));
        t.tl()->setPlayhead(20);
        t.c.setSelectedEdit({a, b});
        t.action("trim.dynamic")->trigger(); // W
        QVERIFY(t.c.dynamicTrim());
        const int before = t.undoCount();
        playing = true; // L: the engine plays and moves the playhead
        for (int64_t f = 21; f <= 25; ++f)
            t.tl()->setPlayhead(f);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:25 25:15")); // the cut rolled along
        playing = false;
        t.c.playbackStopped(); // K
        QCOMPARE(t.undoCount(), before + 1); // one undo step for the run
        // Not playing: moving the playhead does not trim.
        t.tl()->setPlayhead(10);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:25 25:15"));
        t.f.project.undoStack()->undo();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:20 20:20"));
        t.action("trim.dynamic")->trigger();
        QVERIFY(!t.c.dynamicTrim());
    }

    // ---- 13 Transitions ----

    void spec13_5_transitionAlignment()
    {
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(0, 40, 50));
        const quint64 b = t.put(kV1, mediaClip(40, 40, 50));
        Q_UNUSED(a);
        t.c.setSelectedEdit({a, b});
        t.action("timeline.add_transition")->trigger(); // Ctrl+T, 25 frames centred
        QCOMPARE(
            ops::transitionWindow(t.f.view(), b, true), (std::make_pair<int64_t, int64_t>(28, 53)));
        QVERIFY(t.c.setTransitionAlignment(b, true, TransitionAlignment::EndOnCut));
        QCOMPARE(
            ops::transitionWindow(t.f.view(), b, true), (std::make_pair<int64_t, int64_t>(15, 40)));
        // The renderer uses the same window.
        const render::TrackFrame before = render::evaluateTrack(t.f.view().videoTracks[0], 15);
        QVERIFY(before.transition && before.transition->start == 15);
        QVERIFY(!render::evaluateTrack(t.f.view().videoTracks[0], 40).transition);
        QVERIFY(t.c.setTransitionAlignment(b, true, TransitionAlignment::StartOnCut));
        QCOMPARE(
            ops::transitionWindow(t.f.view(), b, true), (std::make_pair<int64_t, int64_t>(40, 65)));
        QVERIFY(!t.c.setTransitionAlignment(b, true, TransitionAlignment::StartOnCut)); // no-op
    }

    void spec13_7_insufficientHandlesPrompt()
    {
        Ctl t;
        // a shows the last 40 frames of its 200-frame source: no tail handle.
        const quint64 a = t.put(kV1, mediaClip(0, 40, 160));
        const quint64 b = t.put(kV1, mediaClip(40, 40, 50));
        t.c.setSelectedEdit({a, b});
        int64_t asked = -1;
        bool shortenOffered = true;
        HandleChoice answer = HandleChoice::Cancel;
        t.c.setHandlePrompt([&](int64_t missing, bool canShorten) {
            asked = missing;
            shortenOffered = canShorten;
            return answer;
        });
        const int before = t.undoCount();
        t.action("timeline.add_transition")->trigger();
        QCOMPARE(asked, int64_t(13)); // 13 of 25 frames lie after the cut
        QVERIFY(!shortenOffered); // no handle at all: nothing shorter fits either
        QCOMPARE(t.undoCount(), before); // cancelled
        answer = HandleChoice::TrimClips;
        t.action("timeline.add_transition")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:27 27:40"));
        QVERIFY(
            !ops::transitionShortfall(t.f.view(), b, true, 25, TransitionAlignment::Centre).any());
        QCOMPARE(ops::clipById(t.f.view(), b)->transitionIn->duration, int64_t(25));
        QCOMPARE(t.undoCount(), before + 1);

        // Shorten: a has 5 frames of tail handle, so a centred transition can be 10 frames.
        Ctl u;
        const quint64 c = u.put(kV1, mediaClip(0, 40, 155));
        const quint64 e = u.put(kV1, mediaClip(40, 40, 50));
        u.c.setSelectedEdit({c, e});
        u.c.setHandlePrompt([&](int64_t, bool canShorten) {
            shortenOffered = canShorten;
            return HandleChoice::Shorten;
        });
        u.action("timeline.add_transition")->trigger();
        QVERIFY(shortenOffered);
        QCOMPARE(ops::clipById(u.f.view(), e)->transitionIn->duration, int64_t(10));
        QCOMPARE(layout(u.f.view(), kV1), QStringLiteral("0:40 40:40"));
    }

    void spec13_9_dragTransitionEdge()
    {
        View v;
        const quint64 a = v.put(kV1, mediaClip(0, 40, 50));
        const quint64 b = v.put(kV1, mediaClip(40, 40, 50));
        v.c.setSelectedEdit({a, b});
        v.action("timeline.add_transition")->trigger();
        v.c.setSelectedEdit({});
        v.c.setTransitionDuration(b, true, 10); // window [35, 45)
        QCOMPARE(
            ops::transitionWindow(v.f.view(), b, true), (std::make_pair<int64_t, int64_t>(35, 45)));
        const int before = v.undoCount();
        v.drag(v.at(45, kV1), v.at(50, kV1)); // the right edge, 5 frames further
        QCOMPARE(ops::clipById(v.f.view(), b)->transitionIn->duration, int64_t(20)); // centred
        QCOMPARE(v.undoCount(), before + 1);
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:40 40:40")); // clips untouched
    }

    void spec13_11_altDragCopiesATransition()
    {
        // Owner request on 0.4.0: Alt+drag a transition block onto another edit point.
        View v;
        v.put(kV1, mediaClip(0, 40, 50));
        const quint64 b = v.put(kV1, mediaClip(40, 40, 50));
        const quint64 c = v.put(kV1, mediaClip(80, 40, 50));
        v.f.project.beginEdit(QStringLiteral("setup"));
        TransitionData t;
        t.duration = 10;
        ops::clipById(v.f.data(), b)->transitionIn = t; // window [35, 45)
        v.f.project.endEdit();
        const int before = v.undoCount();
        v.drag(v.at(40, kV1), v.at(75, kV1), Qt::AltModifier); // onto the tail half of b
        QCOMPARE(v.undoCount(), before + 1);
        QCOMPARE(v.f.project.undoStack()->undoText(), QStringLiteral("Copy Transition"));
        QVERIFY(ops::clipById(v.f.view(), b)->transitionIn); // the original stays
        QVERIFY(!ops::clipById(v.f.view(), b)->transitionOut); // the cut's transition is c's
        QCOMPARE(ops::clipById(v.f.view(), c)->transitionIn->duration, int64_t(10));
        QCOMPARE(v.f.selection.transition(), (TransitionRef {c, true}));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:40 40:40 80:40"));
        // Without Alt the drag does not copy.
        v.drag(v.at(40, kV1), v.at(115, kV1));
        QVERIFY(!ops::clipById(v.f.view(), c)->transitionOut);
    }

    // ---- 16 Clipboard ----

    void spec16_4_pasteAttributes()
    {
        Ctl t;
        ClipData src = solidClip(0, 20);
        src.transform.animated(keys::PosX, 0.0).setKey(5, 100.0);
        src.effects.append(
            EffectRegistry::instance().createEffect(QStringLiteral("gaussian_blur"), 999));
        const quint64 a = t.put(kV1, src);
        t.f.project.beginEdit(QStringLiteral("ids"));
        ops::normalizeIds(t.f.data()); // the hand-made effect id 999
        t.f.project.endEdit();
        ClipData srcAudio = solidClip(0, 20);
        srcAudio.audio.setBase(keys::VolumeDb, -6.0);
        const quint64 aa = t.put(kA1, srcAudio);
        t.link({a, aa});
        const quint64 b = t.put(kV1, solidClip(30, 20));
        const quint64 ba = t.put(kA1, solidClip(30, 20));
        t.link({b, ba});
        t.f.selection.setClips({a});
        t.action("edit.copy")->trigger();
        t.f.selection.setClips({b});
        int asked = 0;
        t.c.setPasteAttributesPrompt([&]() -> std::optional<PasteAttributeSet> {
            ++asked;
            PasteAttributeSet s;
            s.effects = false;
            return s;
        });
        const int before = t.undoCount();
        t.action("edit.paste_attributes")->trigger(); // Alt+V
        QCOMPARE(asked, 1);
        QCOMPARE(t.undoCount(), before + 1);
        QCOMPARE(ops::clipById(t.f.view(), b)->transform, src.transform);
        QVERIFY(ops::clipById(t.f.view(), b)->effects.isEmpty()); // not chosen
        QCOMPARE(ops::clipById(t.f.view(), ba)->audio.value(keys::VolumeDb, 0), -6.0); // linked
        // All groups, with fresh effect ids.
        QVERIFY(t.c.pasteAttributes(PasteAttributeSet {}));
        const auto& fx = ops::clipById(t.f.view(), b)->effects;
        QCOMPARE(fx.size(), 1);
        QVERIFY(fx.front().id != 999);
        QString why;
        QVERIFY2(ops::checkInvariants(t.f.view(), &why), qPrintable(why));
    }

    // ---- 18 Markers ----

    void spec18_4_clipMarkersFollowTheContent()
    {
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(10, 40, 50));
        t.f.selection.setClips({a});
        t.tl()->setPlayhead(30);
        t.action("mark.add_marker")->trigger(); // M on a selected clip: clip marker
        QCOMPARE(t.f.view().markers.size(), 0);
        QCOMPARE(ops::clipById(t.f.view(), a)->markers.size(), 1);
        QCOMPARE(ops::clipById(t.f.view(), a)->markers.front().frame, int64_t(20));
        // A head trim keeps the marker on the same picture.
        t.f.project.beginEdit(QStringLiteral("trim"));
        ops::trimHead(t.f.data(), a, 5, false);
        t.f.project.endEdit();
        QCOMPARE(ops::clipById(t.f.view(), a)->markers.front().frame, int64_t(15));
        // Ctrl+M on the marker opens the editor.
        int edits = 0;
        bool wasClipMarker = false;
        t.c.setMarkerEditor([&](Marker& m, bool clip) {
            ++edits;
            wasClipMarker = clip;
            m.name = QStringLiteral("Look here");
            return true;
        });
        t.action("mark.modify_marker")->trigger();
        QCOMPARE(edits, 1);
        QVERIFY(wasClipMarker);
        QCOMPARE(ops::clipById(t.f.view(), a)->markers.front().name, QStringLiteral("Look here"));
        QCOMPARE(ops::clipById(t.f.view(), a)->markers.size(), 1);
    }

    void spec18_16_mTogglesAMarker()
    {
        // Owner request on 0.4.0: M on an existing marker removes it (one "Delete Marker"
        // step); Ctrl+M still opens the editor.
        Ctl t;
        int edits = 0;
        t.c.setMarkerEditor([&](Marker&, bool) {
            ++edits;
            return false;
        });
        const quint64 a = t.put(kV1, mediaClip(10, 40, 50));
        t.f.selection.setClips({a});
        t.tl()->setPlayhead(30);
        t.action("mark.add_marker")->trigger();
        QCOMPARE(ops::clipById(t.f.view(), a)->markers.size(), 1);
        int before = t.undoCount();
        t.action("mark.add_marker")->trigger(); // clip marker at the playhead: removed
        QCOMPARE(ops::clipById(t.f.view(), a)->markers.size(), 0);
        QCOMPARE(t.undoCount(), before + 1);
        QCOMPARE(t.f.project.undoStack()->undoText(), QStringLiteral("Delete Marker"));
        QCOMPARE(edits, 0);
        t.f.project.undoStack()->undo();
        QCOMPARE(ops::clipById(t.f.view(), a)->markers.size(), 1);

        // No clip selected: timeline markers. A ranged marker toggles at its start frame.
        t.f.selection.clear();
        t.tl()->setPlayhead(60);
        t.action("mark.add_marker")->trigger();
        QCOMPARE(t.f.view().markers.size(), 1);
        t.f.project.beginEdit(QStringLiteral("setup"));
        t.f.data().markers[0].duration = 20;
        t.f.project.endEdit();
        t.tl()->setPlayhead(70); // inside the range, not its start: a second marker
        t.action("mark.add_marker")->trigger();
        QCOMPARE(t.f.view().markers.size(), 2);
        t.tl()->setPlayhead(60);
        before = t.undoCount();
        t.action("mark.add_marker")->trigger();
        QCOMPARE(t.f.view().markers.size(), 1);
        QCOMPARE(t.f.view().markers[0].frame, int64_t(70));
        QCOMPARE(t.undoCount(), before + 1);
        QCOMPARE(t.f.project.undoStack()->undoText(), QStringLiteral("Delete Marker"));
        QCOMPARE(edits, 0);
        // Ctrl+M on an existing marker opens the editor and removes nothing.
        t.tl()->setPlayhead(70);
        t.action("mark.modify_marker")->trigger();
        QCOMPARE(edits, 1);
        QCOMPARE(t.f.view().markers.size(), 1);
    }

    void spec18_5_markerEditor()
    {
        View v;
        v.tl()->setPlayhead(20);
        v.c.setMarkerEditor([](Marker& m, bool clip) {
            if (clip)
                return false;
            m.note = QStringLiteral("check sync");
            m.color = QColor(0xe0, 0x4a, 0x4a);
            return true;
        });
        v.action("mark.modify_marker")->trigger(); // Ctrl+M: add and modify
        QCOMPARE(v.f.view().markers.size(), 1);
        QCOMPARE(v.f.view().markers.front().note, QStringLiteral("check sync"));
        // Double-click on the ruler marker opens the editor again.
        int calls = 0;
        v.c.setMarkerEditor([&](Marker& m, bool) {
            ++calls;
            m.name = QStringLiteral("Renamed");
            return true;
        });
        const QPoint ruler(int(v.w.layout().frameToX(20)), kRulerHeight - 8);
        QTest::mouseDClick(v.vp(), Qt::LeftButton, {}, ruler);
        QCOMPARE(calls, 1);
        QCOMPARE(v.f.view().markers.front().name, QStringLiteral("Renamed"));
    }

    // ---- 21 Moving clips ----

    void spec21_6_shuffleDrag()
    {
        View v;
        const quint64 a = v.put(kV1, solidClip(0, 10, QStringLiteral("a")));
        v.put(kV1, solidClip(10, 20, QStringLiteral("b")));
        v.put(kV1, solidClip(30, 5, QStringLiteral("c")));
        const int before = v.undoCount();
        // Ctrl+Shift drag a to the right: it lands on the edit point between b and c.
        v.drag(v.at(5, kV1), v.at(28, kV1), Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:20 20:10 30:5"));
        QCOMPARE(v.f.view().videoTracks[0].clips[1].id, a);
        QCOMPARE(v.undoCount(), before + 1);
    }

    // ---- 23 Other Edit page commands ----

    void spec23_1_matchFrame()
    {
        Ctl t;
        const QString media = addMedia(t.f.project);
        QString gotMedia;
        int64_t gotUs = -1;
        int64_t gotIn = -1;
        int64_t gotOut = -1;
        t.c.setMatchFrameHandler([&](const QString& id, int64_t us, int64_t in, int64_t out) {
            gotMedia = id;
            gotUs = us;
            gotIn = in;
            gotOut = out;
        });
        ClipData clip = mediaClip(100, 50, 10); // source starts 10 frames in
        clip.mediaId = media;
        t.put(kV1, clip);
        t.tl()->setPlayhead(120);
        t.action("clip.match_frame")->trigger(); // F
        QCOMPARE(gotMedia, media);
        QCOMPARE(gotUs, int64_t(30 * 40000));
        QCOMPARE(gotIn, int64_t(10 * 40000));
        QCOMPARE(gotOut, int64_t(60 * 40000));
    }

    void spec23_2_changeDurationAndSpeed()
    {
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(0, 40, 0)); // 200-frame source
        t.put(kV1, mediaClip(40, 10));
        t.f.selection.setClips({a});
        QVERIFY(t.c.setSelectionDuration(30));
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:30 30:10")); // ripples
        QVERIFY(t.c.setSelectionSpeed(2.0));
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:15 15:10"));
        QCOMPARE(t.f.view().videoTracks[0].clips[0].speed, 2.0);
        QVERIFY(t.c.setSelectionSpeed(0.5)); // 4x longer: 60 frames
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:60 60:10"));
        // The same 30 source frames at 10 %: 300 frames.
        QVERIFY(t.c.setSelectionSpeed(0.1));
        QCOMPARE(t.f.view().videoTracks[0].clips[0].duration, int64_t(300));
        // Longer than the media allows: 200 source frames at 10 % are 2000 frames.
        QVERIFY(t.c.setSelectionDuration(5000));
        QCOMPARE(t.f.view().videoTracks[0].clips[0].duration, int64_t(2000));
    }

    void spec23_3_trackTargetingAndAutoSelectKeys()
    {
        Ctl t;
        t.action("timeline.target_video_2")->trigger(); // Alt+2
        QCOMPARE(t.c.targetVideoTrack(), 1);
        t.action("timeline.target_audio_2")->trigger();
        QCOMPARE(t.c.targetAudioTrack(), 1);
        t.action("timeline.target_video_8")->trigger(); // no V8: ignored
        QCOMPARE(t.c.targetVideoTrack(), 1);
        t.action("timeline.auto_select_video_1")->trigger(); // Ctrl+1
        QVERIFY(!t.f.view().videoTracks[0].autoSelect);
        t.action("timeline.auto_select_audio_2")->trigger();
        QVERIFY(!t.f.view().audioTracks[1].autoSelect);
        QCOMPARE(
            t.action("timeline.target_video_2")->shortcut(), QKeySequence(QStringLiteral("Alt+2")));
    }

    void spec23_4_linkTogglesLinkAndUnlink()
    {
        Ctl t;
        const quint64 v = t.put(kV1, solidClip(0, 10));
        const quint64 a = t.put(kA1, solidClip(0, 10));
        t.f.selection.setClips({v, a});
        t.action("clip.link")->trigger(); // Ctrl+Alt+L
        QVERIFY(ops::clipById(t.f.view(), v)->linkGroup != 0);
        t.action("clip.link")->trigger();
        QCOMPARE(ops::clipById(t.f.view(), v)->linkGroup, quint64(0));
    }
};

QTEST_MAIN(TestParityEdit)
#include "test_parity_edit.moc"
