// SPDX-License-Identifier: GPL-3.0-or-later
// UI and edit tests of spec section 59 (fade curves): the curve handle (dot) on the fade line,
// its hit area and its visibility rule, dragging it (fade-in and fade-out, audio and video
// clips, one undo step, Escape, double-click reset), the drawn curve, linked clips, copy /
// paste / Paste Attributes and split.
#include "../panels/PanelTestUtils.h"
#include "app/Theme.h"
#include "core/LevelOps.h"
#include "ui/common/Resources.h"
#include "ui/timeline/LevelLines.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QApplication>
#include <QImage>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QUndoStack>

#include <cmath>

using namespace kk;
using namespace kk::test;
namespace ll = levellines;

namespace {
constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kA1 {TrackType::Audio, 0};

/// A double-click as a mouse makes it: press, release, double-click, release.
void doubleClick(QWidget* w, const QPoint& at)
{
    const QPoint global = w->mapToGlobal(at);
    for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
             QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, at, global, Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w, &e);
    }
}
} // namespace

class TestFadeCurves : public QObject
{
    Q_OBJECT

    struct View
    {
        PanelFixture f;
        TimelinePanel panel {f.ctx};
        TimelineWidget* w = nullptr;
        quint64 audio = 0;
        quint64 video = 0;
        View()
        {
            panel.controller()->setNamePrompt({});
            panel.controller()->setNewTimelinePrompt({});
            panel.controller()->setConfirmPrompt({});
            panel.controller()->setMessageSink({});
            panel.resize(1400, 600);
            panel.show();
            w = panel.timelineWidget();
            audio = f.addClip(kA1, 0, 200);
            video = f.addClip(kV1, 0, 200);
            w->setPixelsPerFrame(5.0, timelinemetrics::kHeaderWidth);
            w->scrollToFrame(0);
            QApplication::processEvents();
        }
        void setFades(quint64 id, int64_t in, int64_t out)
        {
            f.project.beginEdit(QStringLiteral("Fades"));
            levelops::setFade(f.project.timeline()->mutableData(), id, true, in);
            levelops::setFade(f.project.timeline()->mutableData(), id, false, out);
            f.project.endEdit();
            QApplication::processEvents();
        }
        QRectF rect(quint64 id) const
        {
            const ClipData* c = f.clip(id);
            const TrackRef t = id == audio ? kA1 : kV1;
            return w->layout().clipRect(w->layout().rowOf(t), *c);
        }
        QRectF dot(quint64 id, bool head) const
        {
            return ll::fadeCurveHandleRect(*f.clip(id), rect(id), w->layout(), head);
        }
        int undoCount() const { return f.project.undoStack()->count(); }
        /// Press on the dot, drag by @p dy in steps, optionally release.
        void drag(quint64 id, bool head, int dy, bool release = true)
        {
            QWidget* vp = w->viewport();
            const QPoint start = dot(id, head).center().toPoint();
            QTest::mouseMove(vp, start);
            QTest::mousePress(vp, Qt::LeftButton, {}, start);
            const int step = dy > 0 ? 2 : -2;
            for (int d = step; std::abs(d) <= std::abs(dy); d += step)
                QTest::mouseMove(vp, start + QPoint(0, d));
            if (release)
                QTest::mouseRelease(vp, Qt::LeftButton, {}, start + QPoint(0, dy));
        }
    };

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_fadecurves"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // 59.1: no handle without a fade, none when the fade is narrower than 28 px (5 frames at
    // 5 px per frame is 25 px), one per fade otherwise; the dot sits in the middle of the line.
    void handleVisibilityRule()
    {
        View v;
        for (const quint64 id : {v.audio, v.video}) {
            QVERIFY(v.dot(id, true).isEmpty());
            QVERIFY(v.dot(id, false).isEmpty());
        }
        v.setFades(v.video, 5, 6); // 25 px and 30 px
        QVERIFY(v.dot(v.video, true).isEmpty());
        QVERIFY(!v.dot(v.video, false).isEmpty());
        v.setFades(v.video, 60, 40);
        const QRectF d = v.dot(v.video, true);
        QVERIFY(!d.isEmpty());
        const QRectF r = v.rect(v.video);
        QVERIFY(std::abs(d.center().x() - (r.left() + 150)) < 0.01); // half of 60 frames
        const double top = ll::fadeAreaTop(r);
        const double bottom = ll::fadeAreaBottom(r);
        QVERIFY(std::abs(d.center().y() - (bottom + top) / 2) < 0.01); // linear: half way up
        const QRectF dOut = v.dot(v.video, false);
        QVERIFY(std::abs(dOut.center().x() - (r.right() - 100)) < 0.01); // half of 40 frames
        // The dot never overlaps the fade handle at the fade's end.
        const QRectF handle = ll::fadeHandleRect(*v.f.clip(v.video), r, v.w->layout(), true);
        QVERIFY(!d.adjusted(-2, -2, 2, 2).intersects(handle.adjusted(-2, -2, 2, 2)));
    }

    // 59.1: the hit area is the dot plus 2 px.
    void handleHitArea()
    {
        View v;
        v.setFades(v.audio, 60, 0);
        const QRectF d = v.dot(v.audio, true);
        const ClipData& c = *v.f.clip(v.audio);
        const QRectF r = v.rect(v.audio);
        const QPointF mid = d.center();
        QCOMPARE(ll::hitFadeCurveHandle(c, r, v.w->layout(), mid), 1);
        QCOMPARE(ll::hitFadeCurveHandle(c, r, v.w->layout(), mid + QPointF(5.5, 0)), 1);
        QCOMPARE(ll::hitFadeCurveHandle(c, r, v.w->layout(), mid + QPointF(0, -5.5)), 1);
        QCOMPARE(ll::hitFadeCurveHandle(c, r, v.w->layout(), mid + QPointF(6.5, 0)), 0);
        QCOMPARE(ll::hitFadeCurveHandle(c, r, v.w->layout(), mid + QPointF(0, 6.5)), 0);
        QCOMPARE(ll::hitFadeCurveHandle(c, r, v.w->layout(), r.center() + QPointF(300, 0)), 0);
        // Its own cursor; the fade handle keeps the horizontal one.
        QTest::mouseMove(v.w->viewport(), mid.toPoint());
        QCOMPARE(v.w->viewport()->cursor().shape(), Qt::SplitVCursor);
        const QPoint end = ll::fadeHandleRect(c, r, v.w->layout(), true).center().toPoint();
        QTest::mouseMove(v.w->viewport(), end);
        QCOMPARE(v.w->viewport()->cursor().shape(), Qt::SizeHorCursor);
    }

    // 59.2: dragging down gives a slow start (positive), up a fast start (negative); one undo
    // step; the dot follows the pointer. Fade-in on an audio clip, fade-out on a video clip.
    void dragFadeIn_data()
    {
        QTest::addColumn<int>("dy");
        QTest::addColumn<int>("sign");
        QTest::newRow("down") << 14 << 1;
        QTest::newRow("up") << -14 << -1;
    }
    void dragFadeIn()
    {
        QFETCH(int, dy);
        QFETCH(int, sign);
        View v;
        v.setFades(v.audio, 60, 40);
        const int before = v.undoCount();
        const QPoint start = v.dot(v.audio, true).center().toPoint();
        v.drag(v.audio, true, dy);
        const double curve = v.f.clip(v.audio)->fadeInCurve;
        QVERIFY2(curve * sign > 0.1, qPrintable(QString::number(curve)));
        QCOMPARE(v.f.clip(v.audio)->fadeOutCurve, 0.0); // the other fade is untouched
        QCOMPARE(v.f.clip(v.audio)->fadeInFrames, 60);
        QCOMPARE(v.f.clip(v.audio)->start, 0);
        QCOMPARE(v.undoCount(), before + 1); // one undo step
        // The dot sits where the pointer was released.
        QVERIFY(std::abs(v.dot(v.audio, true).center().y() - (start.y() + dy)) < 1.5);
        v.f.project.undo();
        QCOMPARE(v.f.clip(v.audio)->fadeInCurve, 0.0);
        QCOMPARE(v.undoCount(), before + 1);
    }

    void dragFadeOutOnVideo_data() { dragFadeIn_data(); }
    void dragFadeOutOnVideo()
    {
        QFETCH(int, dy);
        QFETCH(int, sign);
        View v;
        v.setFades(v.video, 30, 50);
        const int before = v.undoCount();
        v.drag(v.video, false, dy);
        const double curve = v.f.clip(v.video)->fadeOutCurve;
        QVERIFY2(curve * sign > 0.1, qPrintable(QString::number(curve)));
        QCOMPARE(v.f.clip(v.video)->fadeInCurve, 0.0);
        QCOMPARE(v.f.clip(v.video)->fadeOutFrames, 50);
        QCOMPARE(v.undoCount(), before + 1);
        v.f.project.undo();
        QCOMPARE(v.f.clip(v.video)->fadeOutCurve, 0.0);
    }

    // The value is limited to -1 ... +1 and a drag back to the middle snaps to linear.
    void dragLimitsAndSnap()
    {
        View v;
        v.setFades(v.video, 60, 0);
        v.drag(v.video, true, 400); // far below the clip
        QCOMPARE(v.f.clip(v.video)->fadeInCurve, 1.0);
        v.drag(v.video, true, -400);
        QCOMPARE(v.f.clip(v.video)->fadeInCurve, -1.0);
        const QPoint start = v.dot(v.video, true).center().toPoint();
        // Press on the dot at -1, move back to where the linear dot would be.
        QWidget* vp = v.w->viewport();
        const QRectF r = v.rect(v.video);
        const double linearY = (ll::fadeAreaTop(r) + ll::fadeAreaBottom(r)) / 2;
        QTest::mousePress(vp, Qt::LeftButton, {}, start);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(
                vp, QPoint(start.x(), start.y() + (int(linearY) - start.y()) * i / 10));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, QPoint(start.x(), int(linearY)));
        QCOMPARE(v.f.clip(v.video)->fadeInCurve, 0.0);
    }

    // 59.3: Escape during the drag cancels it: the value and the undo stack are as before.
    void escapeCancels()
    {
        View v;
        v.setFades(v.audio, 60, 40);
        v.drag(v.audio, true, 10); // set a first curve
        const double first = v.f.clip(v.audio)->fadeInCurve;
        QVERIFY(first > 0.05);
        const int before = v.undoCount();
        v.drag(v.audio, true, -16, false); // dragging, button still down
        QVERIFY(v.f.clip(v.audio)->fadeInCurve < first);
        QTest::keyClick(v.w, Qt::Key_Escape);
        QCOMPARE(v.f.clip(v.audio)->fadeInCurve, first);
        QCOMPARE(v.undoCount(), before);
        QTest::mouseRelease(v.w->viewport(), Qt::LeftButton, {}, QPoint(0, 0));
        QCOMPARE(v.f.clip(v.audio)->fadeInCurve, first);
        QCOMPARE(v.undoCount(), before);
    }

    // 59.4: double-click on the dot resets to linear in one undo step.
    void doubleClickResets()
    {
        View v;
        v.setFades(v.audio, 60, 40);
        v.drag(v.audio, false, 12);
        QVERIFY(v.f.clip(v.audio)->fadeOutCurve > 0.05);
        const int before = v.undoCount();
        doubleClick(v.w->viewport(), v.dot(v.audio, false).center().toPoint());
        QCOMPARE(v.f.clip(v.audio)->fadeOutCurve, 0.0);
        QCOMPARE(v.undoCount(), before + 1);
        QCOMPARE(v.f.clip(v.audio)->fadeOutFrames, 40);
        v.f.project.undo();
        QVERIFY(v.f.clip(v.audio)->fadeOutCurve > 0.05);
        // A double-click on a linear fade changes nothing and adds no undo step.
        v.f.project.undo();
        const int linear = v.undoCount();
        doubleClick(v.w->viewport(), v.dot(v.audio, false).center().toPoint());
        QCOMPARE(v.undoCount(), linear);
    }

    // 59.5: a shorter or removed fade: removing it by its handle also drops the curve.
    void removingTheFadeDropsTheCurve()
    {
        View v;
        v.setFades(v.video, 60, 0);
        v.drag(v.video, true, 10);
        QVERIFY(v.f.clip(v.video)->fadeInCurve != 0.0);
        v.setFades(v.video, 0, 0);
        QCOMPARE(v.f.clip(v.video)->fadeInCurve, 0.0);
        v.setFades(v.video, 30, 0);
        QCOMPARE(v.f.clip(v.video)->fadeInCurve, 0.0);
    }

    // 59.6: the fade handles act on the clip they are on, also for linked clips; so does the
    // curve.
    void linkedClipsKeepTheirOwnCurve()
    {
        View v;
        v.f.project.beginEdit(QStringLiteral("Link"));
        for (const quint64 id : {v.audio, v.video})
            editops::clipById(v.f.project.timeline()->mutableData(), id)->linkGroup = 77;
        v.f.project.endEdit();
        v.setFades(v.video, 60, 0);
        v.setFades(v.audio, 60, 0);
        v.drag(v.video, true, 12);
        QVERIFY(v.f.clip(v.video)->fadeInCurve > 0.05);
        QCOMPARE(v.f.clip(v.audio)->fadeInCurve, 0.0);
    }

    // 59.7: the drawn overlay follows the curve. A fast start (-1) passes through (t = 0.25,
    // 0.707 high), well above the straight line (0.25 high): the line's pixel is there, not on
    // the straight one; with a linear fade it is the other way round.
    void overlayIsDrawnAsTheCurve()
    {
        View v;
        v.setFades(v.video, 100, 0);
        auto lightnessAt = [&](const QImage& img, QPointF p) {
            int best = 0; // the line is thin: the brightest pixel around the point
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    best = std::max(best,
                        qGray(img.pixel(int(std::round(p.x())) + dx, int(std::round(p.y())) + dy)));
            return best;
        };
        const QRectF r = v.rect(v.video);
        const ClipData& c0 = *v.f.clip(v.video);
        const QPointF straight = ll::fadeCurvePoint(c0, r, v.w->layout(), true, 0.25);
        const QImage linear = v.w->viewport()->grab().toImage();
        v.f.project.beginEdit(QStringLiteral("Curve"));
        levelops::setFadeCurve(v.f.project.timeline()->mutableData(), v.video, true, -1.0);
        v.f.project.endEdit();
        QApplication::processEvents();
        const ClipData& c1 = *v.f.clip(v.video);
        const QPointF curved = ll::fadeCurvePoint(c1, r, v.w->layout(), true, 0.25);
        QCOMPARE(curved.x(), straight.x());
        QVERIFY(std::abs((r.bottom() - 1 - curved.y()) / (r.height() - 2) - std::pow(0.25, 0.25))
            < 1e-9);
        QVERIFY(curved.y() < straight.y() - 15);
        const QImage bent = v.w->viewport()->grab().toImage();
        // On the curve the line colour (bright) replaces what was a shaded wedge before.
        QVERIFY2(lightnessAt(bent, curved) > lightnessAt(linear, curved) + 25,
            qPrintable(QStringLiteral("%1 vs %2")
                           .arg(lightnessAt(bent, curved))
                           .arg(lightnessAt(linear, curved))));
        // On the old straight line the pixel is plain clip body now, not the bright line.
        QVERIFY2(lightnessAt(linear, straight) > lightnessAt(bent, straight) + 25,
            qPrintable(QStringLiteral("%1 vs %2")
                           .arg(lightnessAt(linear, straight))
                           .arg(lightnessAt(bent, straight))));
    }

    // 59.8: copy / paste carries the curves with the fade lengths; a split keeps each fade
    // (with its curve) on the side it belongs to; Paste Attributes leaves fades alone (it does
    // not carry fade lengths either).
    void copyPasteSplitAndPasteAttributes()
    {
        View v;
        v.setFades(v.video, 40, 30);
        v.f.project.beginEdit(QStringLiteral("Curves"));
        levelops::setFadeCurve(v.f.project.timeline()->mutableData(), v.video, true, 0.5);
        levelops::setFadeCurve(v.f.project.timeline()->mutableData(), v.video, false, -0.25);
        v.f.project.endEdit();
        TimelineController* ctl = v.panel.controller();
        v.f.selection.setClips({v.video});
        ctl->copy();
        v.f.project.timeline()->setPlayhead(300);
        ctl->paste();
        const quint64 pasted = v.f.selection.currentClip();
        QVERIFY(pasted != 0 && pasted != v.video);
        QCOMPARE(v.f.clip(pasted)->fadeInFrames, 40);
        QCOMPARE(v.f.clip(pasted)->fadeInCurve, 0.5);
        QCOMPARE(v.f.clip(pasted)->fadeOutFrames, 30);
        QCOMPARE(v.f.clip(pasted)->fadeOutCurve, -0.25);
        // Paste Attributes onto a plain clip: no fade and no curve arrive.
        const quint64 plain = v.f.addClip(TrackRef {TrackType::Video, 1}, 0, 50);
        v.f.selection.setClips({plain});
        ctl->pasteAttributes(PasteAttributeSet {});
        QCOMPARE(v.f.clip(plain)->fadeInFrames, 0);
        QCOMPARE(v.f.clip(plain)->fadeInCurve, 0.0);
        QCOMPARE(v.f.clip(plain)->fadeOutCurve, 0.0);
        // Split in the middle: the left part keeps the fade-in curve, the right the fade-out's.
        v.f.project.beginEdit(QStringLiteral("Split"));
        const QVector<quint64> right
            = editops::splitClips(v.f.project.timeline()->mutableData(), {v.video}, 100);
        v.f.project.endEdit();
        QCOMPARE(right.size(), 1);
        QCOMPARE(v.f.clip(v.video)->fadeInCurve, 0.5);
        QCOMPARE(v.f.clip(v.video)->fadeOutCurve, 0.0);
        QCOMPARE(v.f.clip(right.front())->fadeInCurve, 0.0);
        QCOMPARE(v.f.clip(right.front())->fadeOutCurve, -0.25);
    }

    // 59.13: after a cut the fade-out of the short right piece lies inside it, its handle can be
    // grabbed and dragged, and drawing clamps even bad data.
    void splitLeavesGrabbableHandle()
    {
        View v;
        v.setFades(v.video, 0, 48);
        v.f.project.beginEdit(QStringLiteral("Split"));
        const QVector<quint64> right
            = editops::splitClips(v.f.project.timeline()->mutableData(), {v.video}, 180);
        v.f.project.endEdit();
        QApplication::processEvents();
        QCOMPARE(right.size(), 1);
        const quint64 r = right.front();
        QCOMPARE(v.f.clip(r)->fadeOutFrames, 20);
        QCOMPARE(v.f.clip(v.video)->fadeOutFrames, 0);
        const QRectF clipRect = v.w->layout().clipRect(v.w->layout().rowOf(kV1), *v.f.clip(r));
        const QRectF handle = ll::fadeHandleRect(*v.f.clip(r), clipRect, v.w->layout(), false);
        QVERIFY(!handle.isEmpty());
        QVERIFY(clipRect.adjusted(-1, -1, 1, 1).contains(handle.center())); // it may straddle the clip edge
        QVERIFY(levelops::setFade(v.f.project.timeline()->mutableData(), r, false, 10));
        QCOMPARE(v.f.clip(r)->fadeOutFrames, 10);
        // Bad data (fade longer than the clip, as an old file had) is drawn inside the clip.
        ClipData bad = *v.f.clip(r);
        bad.fadeOutFrames = 500;
        const QRectF h = ll::fadeHandleRect(bad, clipRect, v.w->layout(), false);
        QVERIFY(clipRect.adjusted(-1, -1, 1, 1).contains(h.center()));
    }
};

QTEST_MAIN(TestFadeCurves)
#include "test_fadecurves.moc"
