// SPDX-License-Identifier: GPL-3.0-or-later
// Display animations of the timeline (v2.2, spec 36): with animations off the view shows the
// model at once; on, clips tween from their old place and land exactly on the model; hit tests
// use the model mid-animation; a new edit restarts from the displayed state; blade flashes,
// ghosts, playhead glides and the snap fade; the preference persists and applies live.
// Time is driven by a test clock (uianim::setClockForTesting), never by waiting.
#include "TimelineTestUtils.h"
#include "core/Preferences.h"
#include "core/Timeline.h"
#include "ui/common/UiAnimation.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/timeline/TimelineAnimator.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QCheckBox>
#include <QDir>
#include <QPainter>
#include <QSlider>
#include <QTest>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {
constexpr TrackRef kV1 {TrackType::Video, 0};

int64_t g_now = 0; ///< the test clock (ms)

/// Fixture + controller + a shown widget at 10 px per frame.
struct View
{
    TimelineFixture f;
    TimelineController c {f.ctx};
    TimelineWidget w {f.ctx, &c};

    View()
    {
        c.connectActions();
        w.setThumbnailProvider({});
        w.setWaveformProvider({});
        w.resize(1200, 400);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(10.0, kHeaderWidth);
        w.scrollToFrame(0);
    }
    quint64 put(ClipData clip)
    {
        f.project.beginEdit(QStringLiteral("setup"));
        const quint64 id = ops::overwriteClip(f.data(), kV1, clip);
        f.project.endEdit();
        return id;
    }
    void rippleDelete(quint64 id)
    {
        f.project.beginEdit(QStringLiteral("ripple"));
        ops::rippleDeleteClips(f.data(), {id});
        f.project.endEdit();
    }
    /// Model rect of clip @p id.
    QRectF modelRect(quint64 id)
    {
        const auto loc = ops::findClip(f.view(), id);
        const TimelineLayout& L = w.layout();
        return L.clipRect(
            L.rowOf(loc->track), f.view().track(loc->track)->clips[size_t(loc->clipIndex)]);
    }
    /// Displayed rect of clip @p id at the current test time.
    QRectF shown(quint64 id)
    {
        QRectF r = modelRect(id);
        w.animator().displayClip(id, r, w.layout());
        return r;
    }
    /// Runs one paint (tick) at test time @p ms; returns whether anything animates.
    bool at(int64_t ms)
    {
        g_now = ms;
        return w.animator().tick();
    }
    /// Renders the viewport into an image at test time @p ms.
    QImage grab(int64_t ms)
    {
        g_now = ms;
        return w.viewport()->grab().toImage();
    }
};
} // namespace

class TestTimelineAnimation : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qputenv("KK_ANIMATIONS", "1"); // offscreen defaults to off
        initTimelineTestCase("test_timelineanimation");
        uianim::setClockForTesting([] { return g_now; });
        uianim::reload();
    }
    void init()
    {
        g_now = 0;
        uianim::setSpeed(1.0);
        uianim::setEnabled(true);
    }
    void cleanupTestCase() { uianim::setClockForTesting({}); }

    void disabledShowsTheModelAtOnce()
    {
        uianim::setEnabled(false);
        View v;
        const quint64 a = v.put(solidClip(0, 20));
        const quint64 b = v.put(solidClip(20, 20));
        v.rippleDelete(a);
        QVERIFY(!v.at(0));
        QCOMPARE(v.shown(b), v.modelRect(b));
        QVERIFY(v.w.animator().ghosts().empty());
        QCOMPARE(v.w.animator().playheadFrame(7), 7.0);
    }

    void rippleDeleteSlidesAndLandsOnTheModel()
    {
        View v;
        const quint64 a = v.put(solidClip(0, 20));
        const quint64 b = v.put(solidClip(20, 20));
        const QRectF before = v.modelRect(b);
        v.rippleDelete(a);
        const QRectF after = v.modelRect(b);
        QCOMPARE(after.left(), before.left() - 200.0);
        // Start: still where it was; middle: in between; end: exactly the model.
        QVERIFY(v.at(0));
        QCOMPARE(v.shown(b).left(), before.left());
        QVERIFY(v.at(uianim::duration(uianim::Kind::Move) / 2));
        const double mid = v.shown(b).left();
        QVERIFY2(mid < before.left() && mid > after.left(), qPrintable(QString::number(mid)));
        // OutCubic: more than half the way after half the time.
        QVERIFY(mid < (before.left() + after.left()) / 2.0);
        // The removed clip fades out as a ghost.
        QCOMPARE(v.w.animator().ghosts().size(), size_t(1));
        QVERIFY(!v.at(1000));
        QCOMPARE(v.shown(b), after);
        QVERIFY(v.w.animator().ghosts().empty());
    }

    void hitTestsUseTheModelMidAnimation()
    {
        View v;
        const quint64 a = v.put(solidClip(0, 20));
        const quint64 b = v.put(solidClip(20, 20));
        v.rippleDelete(a);
        QVERIFY(v.at(40));
        // Frame 5 is clip b in the model, although b is still drawn further right.
        const TimelineLayout& L = v.w.layout();
        const QPointF p(L.frameToX(5.0), L.rowRect(L.rowOf(kV1)).center().y());
        QVERIFY(v.shown(b).left() > p.x());
        const TimelineHit hit = L.hitTest(v.f.view(), p);
        QCOMPARE(hit.clipId, b);
    }

    void newEditRestartsFromTheDisplayedState()
    {
        View v;
        const quint64 a = v.put(solidClip(0, 20));
        const quint64 b = v.put(solidClip(20, 20));
        v.rippleDelete(a);
        QVERIFY(v.at(60));
        const QRectF mid = v.shown(b);
        // Undo mid-animation: b goes back to 20, starting from where it is drawn now.
        v.f.project.undo();
        QVERIFY(v.at(60));
        QVERIFY(qAbs(v.shown(b).left() - mid.left()) < 0.5);
        QVERIFY(v.at(60 + uianim::duration(uianim::Kind::Move) / 2));
        QVERIFY(v.shown(b).left() > mid.left());
        QVERIFY(!v.at(2000));
        QCOMPARE(v.shown(b), v.modelRect(b));
        // The undone clip a slides back from its ghost instead of popping in.
        QVERIFY(v.w.animator().ghosts().empty());
    }

    void bladeFlashesAndSeparatesTheHalves()
    {
        View v;
        const quint64 a = v.put(solidClip(0, 40));
        v.f.project.beginEdit(QStringLiteral("blade"));
        const QVector<quint64> right = ops::splitClips(v.f.data(), {a}, 20);
        v.f.project.endEdit();
        QCOMPARE(right.size(), 1);
        QVERIFY(v.at(0));
        QCOMPARE(v.w.animator().flashes().size(), size_t(1));
        QCOMPARE(v.w.animator().flashes().front().frame, int64_t(20));
        // The halves are drawn a hair apart at the cut, then settle.
        const double cut = v.w.layout().frameToX(20.0);
        QVERIFY(v.shown(a).right() < cut - 1.0);
        QVERIFY(v.shown(right.front()).left() > cut + 1.0);
        // No piece fades in: the picture was there before.
        QRectF r = v.modelRect(right.front());
        QCOMPARE(v.w.animator().displayClip(right.front(), r, v.w.layout()), 1.0);
        QVERIFY(!v.at(1000));
        QCOMPARE(v.shown(a), v.modelRect(a));
        QVERIFY(v.w.animator().flashes().empty());
    }

    void insertedClipAppears()
    {
        View v;
        v.put(solidClip(0, 20));
        const quint64 c = v.put(solidClip(50, 10));
        QVERIFY(v.at(0));
        QRectF r = v.modelRect(c);
        const double opacity = v.w.animator().displayClip(c, r, v.w.layout());
        QVERIFY(opacity < 0.5);
        QVERIFY(r.height() < v.modelRect(c).height());
        QVERIFY(!v.at(1000));
        QCOMPARE(v.shown(c), v.modelRect(c));
    }

    void liveDragsAreNotAnimated()
    {
        View v;
        const quint64 a = v.put(solidClip(10, 20));
        QVERIFY(!v.at(1000));
        const TimelineLayout& L = v.w.layout();
        const int y = L.rowRect(L.rowOf(kV1)).center().y();
        const QPoint from(int(L.frameToX(20.0)), y);
        const QPoint to(int(L.frameToX(50.0)), y);
        QTest::mousePress(v.w.viewport(), Qt::LeftButton, {}, from);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(v.w.viewport(), from + (to - from) * i / 4);
        QTest::mouseRelease(v.w.viewport(), Qt::LeftButton, {}, to);
        QCOMPARE(v.f.view().track(kV1)->clips.front().start, int64_t(40));
        // The drag was 1:1 with the pointer: nothing slides afterwards.
        v.at(1000);
        QVERIFY(!v.w.animator().isAnimating(a));
        QCOMPARE(v.shown(a), v.modelRect(a));
        // ...and the next committed edit (undo) animates again.
        v.f.project.undo();
        QVERIFY(v.at(1000 + 10));
        QVERIFY(v.w.animator().isAnimating(a));
    }

    void playheadJumpGlides()
    {
        View v;
        v.put(solidClip(0, 100));
        Timeline* tl = v.f.project.timeline();
        g_now = 1000;
        tl->setPlayhead(80);
        QVERIFY(v.at(1000 + uianim::duration(uianim::Kind::Glide) / 2));
        const double mid = v.w.animator().playheadFrame(80);
        QVERIFY2(mid > 0.0 && mid < 80.0, qPrintable(QString::number(mid)));
        QVERIFY(!v.at(3000));
        QCOMPARE(v.w.animator().playheadFrame(80), 80.0);
        // Changes in quick succession (a drag, key repeat) are instant.
        g_now = 3010;
        tl->setPlayhead(40);
        g_now = 3030;
        tl->setPlayhead(60);
        QVERIFY(!v.at(3040));
        QCOMPARE(v.w.animator().playheadFrame(60), 60.0);
    }

    void snapIndicatorFades()
    {
        View v;
        TimelineAnimator& anim = v.w.animator();
        g_now = 0;
        anim.observeSnap(int64_t(12));
        anim.tick();
        QCOMPARE(anim.snapLine(int64_t(12))->second, 0.0);
        g_now = uianim::duration(uianim::Kind::Fade) / 2;
        anim.tick();
        QVERIFY(anim.snapLine(int64_t(12))->second > 0.3);
        g_now = 500;
        anim.observeSnap(std::nullopt);
        anim.tick();
        const auto out = anim.snapLine(std::nullopt);
        QVERIFY(out && out->first == 12 && out->second > 0.9);
        g_now = 2000;
        anim.tick();
        QVERIFY(!anim.snapLine(std::nullopt));
    }

    void preferencePersistsAndAppliesLive()
    {
        View v;
        const quint64 a = v.put(solidClip(0, 20));
        const quint64 b = v.put(solidClip(20, 20));
        {
            PreferencesDialog dialog(nullptr);
            QVERIFY(dialog.showSection(QStringLiteral("ui")));
            auto* box = dialog.findChild<QCheckBox*>(QStringLiteral("AnimateTimelineEdits"));
            auto* speed = dialog.findChild<QSlider*>(QStringLiteral("AnimationSpeed"));
            QVERIFY(box && speed);
            QVERIFY(box->isChecked()); // the default
            box->setChecked(false);
            dialog.apply();
        }
        QVERIFY(!prefs::animationsEnabled());
        QVERIFY(!uianim::enabled());
        v.rippleDelete(a);
        QVERIFY(!v.w.animator().isAnimating(b));
        {
            PreferencesDialog dialog(nullptr);
            auto* box = dialog.findChild<QCheckBox*>(QStringLiteral("AnimateTimelineEdits"));
            auto* speed = dialog.findChild<QSlider*>(QStringLiteral("AnimationSpeed"));
            QVERIFY(!box->isChecked()); // persisted
            box->setChecked(true);
            speed->setValue(speed->maximum()); // 2x
            dialog.apply();
        }
        QVERIFY(prefs::animationsEnabled());
        QCOMPARE(prefs::animationSpeed(), 2.0);
        QVERIFY(uianim::enabled());
        QCOMPARE(uianim::duration(uianim::Kind::Move), 90);
        // Turned on again live: the next edit animates.
        v.f.project.undo();
        QVERIFY(v.at(10));
        QVERIFY(v.w.animator().isAnimating(b));
        // Turned off mid-animation: everything stops at once.
        uianim::setEnabled(false);
        QVERIFY(!v.w.animator().active());
        prefs::setAnimationSpeed(1.0);
    }

    /// Demo frames for the release notes (KK_ANIMATION_SHOTS=<folder>): a ripple delete and a
    /// blade cut, grabbed at fixed test times.
    void demoFrames()
    {
        const QString folder = qEnvironmentVariable("KK_ANIMATION_SHOTS");
        if (folder.isEmpty())
            QSKIP("set KK_ANIMATION_SHOTS to write the demo frames");
        QDir().mkpath(folder);
        View v;
        v.w.setPixelsPerFrame(6.0, kHeaderWidth);
        const quint64 a = v.put(solidClip(0, 40, QStringLiteral("Intro")));
        v.put(solidClip(40, 60, QStringLiteral("Interview")));
        v.put(solidClip(100, 50, QStringLiteral("B-roll")));
        v.at(5000);
        int n = 0;
        auto shoot = [&](const char* name, int64_t t0, int frames, int stepMs) {
            for (int i = 0; i < frames; ++i) {
                const QRect row = v.w.layout().rowRect(v.w.layout().rowOf(kV1));
                const QImage img
                    = v.grab(t0 + i * stepMs).copy(0, row.top() - 24, 760, row.height() + 34);
                QVERIFY(img.save(QStringLiteral("%1/%2_%3.png")
                                     .arg(folder, QLatin1String(name))
                                     .arg(n++, 3, 10, QLatin1Char('0'))));
            }
        };
        g_now = 10000;
        v.rippleDelete(a);
        shoot("ripple", 10000, 12, 20);
        const quint64 interview = v.f.view().track(kV1)->clips.front().id;
        g_now = 20000;
        v.f.project.beginEdit(QStringLiteral("blade"));
        ops::splitClips(v.f.data(), {interview}, 30);
        v.f.project.endEdit();
        shoot("blade", 20000, 12, 20);
    }
};

QTEST_MAIN(TestTimelineAnimation)
#include "test_timelineanimation.moc"
