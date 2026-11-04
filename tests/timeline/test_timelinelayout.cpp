// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for TimelineLayout: frame <-> x at several zooms, zoom anchoring, row stacking,
// clip range searches and hit-testing (body, edges, edit points, header buttons, resize).
#include "core/EditOps.h"
#include "ui/timeline/TimelineLayout.h"

#include <QTest>

using namespace kk;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {
ClipData clip(int64_t start, int64_t duration)
{
    ClipData c;
    c.start = start;
    c.duration = duration;
    return c;
}

TimelineData timeline()
{
    return makeDefaultTimeline(QStringLiteral("T"), Rational {25, 1}, 1920, 1080, 2, 2);
}

TimelineLayout layoutFor(const TimelineData& d, double ppf, QSize size = {1200, 600})
{
    TimelineLayout L;
    L.setViewSize(size);
    L.rebuild(d);
    L.setPixelsPerFrame(ppf, kHeaderWidth);
    L.setScrollX(0);
    return L;
}
} // namespace

class TestTimelineLayout : public QObject
{
    Q_OBJECT
private slots:
    void frameToXRoundTrip_data()
    {
        QTest::addColumn<double>("ppf");
        QTest::addColumn<double>("scroll");
        QTest::newRow("far out") << 0.01 << 0.0;
        QTest::newRow("fit-ish") << 0.5 << 37.0;
        QTest::newRow("1:1") << 1.0 << 250.0;
        QTest::newRow("close") << 12.0 << 1000.0;
        QTest::newRow("max") << kMaxPixelsPerFrame << 48000.0;
    }
    void frameToXRoundTrip()
    {
        QFETCH(double, ppf);
        QFETCH(double, scroll);
        TimelineLayout L = layoutFor(timeline(), ppf);
        L.setScrollX(scroll);
        for (double f : {0.0, 1.0, 24.5, 1000.0, 123456.0}) {
            const double x = L.frameToX(f);
            QCOMPARE(x, kHeaderWidth + f * ppf - scroll);
            QVERIFY(qAbs(L.xToFrame(x) - f) < 1e-9);
        }
    }

    void frameAtXFloorsAndNearestRounds()
    {
        TimelineLayout L = layoutFor(timeline(), 10.0);
        QCOMPARE(L.frameAtX(kHeaderWidth + 0), int64_t(0));
        QCOMPARE(L.frameAtX(kHeaderWidth + 9.9), int64_t(0));
        QCOMPARE(L.frameAtX(kHeaderWidth + 10), int64_t(1));
        QCOMPARE(L.nearestFrameAtX(kHeaderWidth + 14), int64_t(1));
        QCOMPARE(L.nearestFrameAtX(kHeaderWidth + 16), int64_t(2));
        QCOMPARE(L.frameAtX(0), int64_t(0)); // clamped, never negative
        QCOMPARE(L.nearestFrameAtX(-500), int64_t(0));
    }

    void zoomKeepsAnchorFrame()
    {
        TimelineLayout L = layoutFor(timeline(), 2.0);
        L.setScrollX(3000);
        const double anchorX = 700;
        const double before = L.xToFrame(anchorX);
        L.setPixelsPerFrame(7.5, anchorX);
        QVERIFY(qAbs(L.xToFrame(anchorX) - before) < 1e-9);
        L.setPixelsPerFrame(0.3, anchorX);
        QVERIFY(qAbs(L.xToFrame(anchorX) - before) < 1e-6);
        // Zooming out so far that frame 0 would move right of the header clamps the scroll.
        L.setPixelsPerFrame(0.05, anchorX);
        QCOMPARE(L.scrollX(), 0.0);
        // Limits.
        L.setPixelsPerFrame(1e9, anchorX);
        QCOMPARE(L.pixelsPerFrame(), kMaxPixelsPerFrame);
        L.setPixelsPerFrame(0, anchorX);
        QCOMPARE(L.pixelsPerFrame(), kMinPixelsPerFrame);
    }

    void fitAndVisibleFrames()
    {
        TimelineLayout L = layoutFor(timeline(), 1.0, {1200, 600});
        const double ppf = L.fitPixelsPerFrame(1000);
        QVERIFY(ppf * 1000 <= L.trackAreaWidth());
        QVERIFY(ppf * 1000 > L.trackAreaWidth() * 0.9);
        L.setPixelsPerFrame(2.0, kHeaderWidth);
        L.setScrollX(100);
        const auto [f0, f1] = L.visibleFrames();
        QCOMPARE(f0, int64_t(50));
        QCOMPARE(f1, int64_t(std::ceil((1200 - kHeaderWidth + 100) / 2.0)));
    }

    void rowsStackVideoAboveAudio()
    {
        TimelineData d = timeline();
        ops::addTrack(d, TrackType::Video);
        d.audioTracks[1].height = 80;
        TimelineLayout L = layoutFor(d, 1.0, {1200, 2000});
        const auto& rows = L.rows();
        QCOMPARE(rows.size(), size_t(5));
        QCOMPARE(rows[0].ref, (TrackRef {TrackType::Video, 2})); // V3 on top
        QCOMPARE(rows[2].ref, (TrackRef {TrackType::Video, 0})); // V1 next to the divider
        QCOMPARE(rows[3].ref, (TrackRef {TrackType::Audio, 0})); // A1 below the divider
        QCOMPARE(rows[4].height, 80);
        QCOMPARE(rows[3].top, 3 * kDefaultVideoHeight + kSectionGap);
        QCOMPARE(
            L.contentHeight(), 3 * kDefaultVideoHeight + kSectionGap + kDefaultAudioHeight + 80);
        // Content fits: the divider is centred in the row area.
        const int area = L.rowAreaHeight();
        QCOMPARE(L.dividerY(), kRulerHeight + area / 2);
        // rowAtY / rowOf agree with rowRect.
        for (int i = 0; i < int(rows.size()); ++i) {
            const QRect r = L.rowRect(i);
            QCOMPARE(L.rowAtY(r.top()), i);
            QCOMPARE(L.rowAtY(r.bottom()), i);
            QCOMPARE(L.rowOf(rows[size_t(i)].ref), i);
        }
        QCOMPARE(L.rowAtY(L.dividerY() + 1), -1); // the divider is no row
        QCOMPARE(L.rowAtY(5), -1); // ruler
    }

    void verticalScrollWhenOverflowing()
    {
        TimelineData d = timeline();
        for (int i = 0; i < 10; ++i)
            ops::addTrack(d, TrackType::Video);
        TimelineLayout L = layoutFor(d, 1.0, {1200, 300});
        QCOMPARE(L.verticalOffset(), 0);
        QVERIFY(L.maxScrollY() > 0);
        L.setScrollY(40);
        QCOMPARE(L.rowRect(0).top(), kRulerHeight - 40);
        QCOMPARE(L.rowAtY(kRulerHeight), 0);
        L.setScrollY(100000);
        QCOMPARE(L.scrollY(), L.maxScrollY());
    }

    void clipRangeBinarySearch()
    {
        TrackData t;
        for (int i = 0; i < 100; ++i)
            t.clips.push_back(clip(i * 10, 5)); // [0,5) [10,15) ...
        auto range = TimelineLayout::clipRange(t, 12, 33);
        QCOMPARE(range.first, size_t(1)); // [10,15)
        QCOMPARE(range.second, size_t(4)); // up to [30,35)
        range = TimelineLayout::clipRange(t, 15, 20);
        QCOMPARE(range.first, range.second); // only a gap
        range = TimelineLayout::clipRange(t, -100, 1e9);
        QCOMPARE(range.first, size_t(0));
        QCOMPARE(range.second, size_t(100));
        range = TimelineLayout::clipRange(t, 2000, 3000);
        QCOMPARE(range.first, size_t(100));
    }

    void hitTestClipParts()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, TrackRef {TrackType::Video, 0}, clip(10, 20));
        const quint64 b = ops::overwriteClip(d, TrackRef {TrackType::Video, 0}, clip(30, 20));
        const quint64 c = ops::overwriteClip(d, TrackRef {TrackType::Video, 0}, clip(60, 10));
        TimelineLayout L = layoutFor(d, 10.0);
        const int row = L.rowOf(TrackRef {TrackType::Video, 0});
        const double y = L.rowRect(row).center().y();

        TimelineHit h = L.hitTest(d, {L.frameToX(20), y});
        QCOMPARE(h.area, TimelineHit::Area::Clip);
        QCOMPARE(h.clipId, a);
        QCOMPARE(h.part, ClipPart::Body);

        h = L.hitTest(d, {L.frameToX(10) + 2, y});
        QCOMPARE(h.part, ClipPart::Head);
        QCOMPARE(h.clipId, a);
        QCOMPARE(h.cutLeft, quint64(0)); // gap before a: no edit point

        // The shared edge of a and b: the side the pointer is on wins, both ids reported.
        h = L.hitTest(d, {L.frameToX(30) - 2, y});
        QCOMPARE(h.clipId, a);
        QCOMPARE(h.part, ClipPart::Tail);
        QCOMPARE(h.cutLeft, a);
        QCOMPARE(h.cutRight, b);
        h = L.hitTest(d, {L.frameToX(30) + 2, y});
        QCOMPARE(h.clipId, b);
        QCOMPARE(h.part, ClipPart::Head);
        QCOMPARE(h.cutLeft, a);
        QCOMPARE(h.cutRight, b);

        // Slightly outside a tail into a gap still grabs the edge.
        h = L.hitTest(d, {L.frameToX(50) + 1.5, y});
        QCOMPARE(h.clipId, b);
        QCOMPARE(h.part, ClipPart::Tail);
        QCOMPARE(h.cutRight, quint64(0));
        // Deep in the gap: empty track.
        h = L.hitTest(d, {L.frameToX(55), y});
        QCOMPARE(h.area, TimelineHit::Area::Track);
        QCOMPARE(h.clipId, quint64(0));
        QCOMPARE(h.track, (TrackRef {TrackType::Video, 0}));
        QVERIFY(qAbs(h.frame - 55) < 1e-9);
        h = L.hitTest(d, {L.frameToX(65), y});
        QCOMPARE(h.clipId, c);
    }

    void hitTestShortClipKeepsBody()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, TrackRef {TrackType::Video, 0}, clip(10, 3));
        TimelineLayout L = layoutFor(d, 5.0); // 15 px wide: 5 px edge zones
        const double y = L.rowRect(L.rowOf(TrackRef {TrackType::Video, 0})).center().y();
        const TimelineHit h = L.hitTest(d, {L.frameToX(11.5), y});
        QCOMPARE(h.clipId, a);
        QCOMPARE(h.part, ClipPart::Body);
    }

    void hitTestRulerAndHeaders()
    {
        TimelineData d = timeline();
        TimelineLayout L = layoutFor(d, 1.0);
        QCOMPARE(L.hitTest(d, {500, 5}).area, TimelineHit::Area::Ruler);
        QCOMPARE(L.hitTest(d, {50, 5}).area, TimelineHit::Area::None);
        const int row = L.rowOf(TrackRef {TrackType::Audio, 0});
        const QRect r = L.rowRect(row);
        TimelineHit h = L.hitTest(d, {100, double(r.top() + 20)});
        QCOMPARE(h.area, TimelineHit::Area::Header);
        QCOMPARE(h.track, (TrackRef {TrackType::Audio, 0}));
        QCOMPARE(L.hitTest(d, {100, double(r.bottom())}).area, TimelineHit::Area::HeaderResize);
        for (HeaderButton b :
            {HeaderButton::Target, HeaderButton::Lock, HeaderButton::Mute, HeaderButton::Solo}) {
            const QRect br = L.headerButtonRect(row, b);
            QVERIFY(!br.isEmpty());
            QVERIFY(br.right() < kHeaderWidth);
            QCOMPARE(L.hitTest(d, br.center()).button, b);
        }
        // Video tracks have no mute/solo but a visibility button.
        const int vRow = L.rowOf(TrackRef {TrackType::Video, 0});
        QVERIFY(L.headerButtonRect(vRow, HeaderButton::Mute).isEmpty());
        QCOMPARE(L.hitTest(d, L.headerButtonRect(vRow, HeaderButton::Visible).center()).button,
            HeaderButton::Visible);
    }
};

QTEST_GUILESS_MAIN(TestTimelineLayout)
#include "test_timelinelayout.moc"
