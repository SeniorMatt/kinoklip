// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the Cut page smart edits (core/CutEdits.h): smart insert at the nearest edit
// point, append, ripple overwrite, place on top, close up, source overwrite by timecode and the
// fast review speed. Item numbers refer to docs/behavior-spec.md section 35.
#include "CoreTestUtils.h"
#include "core/CutEdits.h"
#include "core/EditOps.h"
#include "core/ParamKeys.h"

#include <QTest>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;
namespace cut = kk::cutedits;

namespace {

/// One segment of @p frames: video on V1 and audio on A1 (offset @p offset from the first).
QVector<ops::PlacedClip> segment(int64_t offset, int64_t frames, const QString& media)
{
    ClipData c = mediaClip(offset, frames, 0, 1000, media);
    c.mediaId = media;
    return {{kV1, c}, {kA1, c}};
}

/// A clip of media @p media on @p ref.
quint64 put(TimelineData& d, TrackRef ref, int64_t start, int64_t duration,
    const QString& media = QStringLiteral("base"))
{
    ClipData c = mediaClip(start, duration, 0, 1000, media);
    c.mediaId = media;
    return ops::overwriteClip(d, ref, c);
}

} // namespace

class TestCutEdits : public QObject
{
    Q_OBJECT
private slots:
    // ---- 35.6 smart insert ----
    void nearestEditPointPicksClosestCut()
    {
        TimelineData d = timeline();
        put(d, kV1, 0, 50);
        put(d, kV1, 50, 30); // cuts at 0, 50, 80
        put(d, kA1, 0, 64); // A1 cut at 64
        QCOMPARE(cut::nearestEditPoint(d, 10), int64_t(0));
        QCOMPARE(cut::nearestEditPoint(d, 30), int64_t(50));
        QCOMPARE(cut::nearestEditPoint(d, 60), int64_t(64)); // other tracks count too
        QCOMPARE(cut::nearestEditPoint(d, 75), int64_t(80));
        QCOMPARE(cut::nearestEditPoint(d, 500), int64_t(80)); // timeline end
        QCOMPARE(cut::nearestEditPoint(d, 25), int64_t(0)); // tie: the earlier one
        QCOMPARE(cut::nearestEditPoint(timeline(), 40), int64_t(0)); // empty timeline
    }

    void smartInsertNeverSplitsAClip()
    {
        TimelineData d = timeline();
        put(d, kV1, 0, 50);
        put(d, kV1, 50, 30);
        put(d, kA1, 0, 50);
        put(d, kA1, 50, 30);
        const cut::Result r = cut::smartInsert(d, {segment(0, 20, QStringLiteral("new"))}, 40);
        QVERIFY(r.ok());
        QCOMPARE(r.start, int64_t(50)); // 40 is nearer to 50 than to 0
        QCOMPARE(r.end, int64_t(70));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:50 50:20 70:30"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:50 50:20 70:30"));
        QCOMPARE(r.ids.size(), 2);
        QCOMPARE(ops::linkedClips(d, r.ids[0]).size(), 2); // video + audio linked
        KK_VERIFY_INVARIANTS(d);
    }

    void smartInsertSeveralSegmentsBackToBack()
    {
        TimelineData d = timeline();
        put(d, kV1, 0, 40);
        cut::SourceGroups groups {
            segment(0, 10, QStringLiteral("a")), segment(10, 15, QStringLiteral("b"))};
        const cut::Result r = cut::smartInsert(d, groups, 3);
        QCOMPARE(r.start, int64_t(0));
        QCOMPARE(r.end, int64_t(25));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:15 25:40"));
        QCOMPARE(r.ids.size(), 4);
        // Each segment is linked on its own (a's video with a's audio).
        const QVector<quint64> linkedA = ops::linkedClips(d, r.ids[0]);
        QCOMPARE(linkedA.size(), 2);
        QVERIFY(linkedA.contains(r.ids[1]));
        QVERIFY(!linkedA.contains(r.ids[2]));
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- 35.7 append ----
    void appendGoesAfterTheLastClip()
    {
        TimelineData d = timeline();
        put(d, kV1, 0, 30);
        put(d, kA2, 0, 45); // the timeline ends at 45 (any track)
        const cut::Result r = cut::append(d, {segment(0, 10, QStringLiteral("n"))});
        QVERIFY(r.ok());
        QCOMPARE(r.start, int64_t(45));
        QCOMPARE(r.end, int64_t(55));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:30 45:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("45:10"));
        QCOMPARE(layout(d, kA2), QStringLiteral("0:45")); // nothing moved
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- 35.8 ripple overwrite ----
    void rippleOverwriteReplacesClipUnderPlayhead()
    {
        TimelineData d = timeline();
        put(d, kV1, 0, 20);
        put(d, kV1, 20, 30); // replaced
        put(d, kV1, 50, 10);
        put(d, kA1, 0, 20);
        put(d, kA1, 20, 30);
        put(d, kA1, 50, 10);
        const cut::Result r = cut::rippleOverwrite(d, {segment(0, 12, QStringLiteral("n"))}, 33);
        QVERIFY(r.ok());
        QCOMPARE(r.start, int64_t(20));
        QCOMPARE(r.end, int64_t(32));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:20 20:12 32:10")); // later clip rippled left
        QCOMPARE(layout(d, kA1), QStringLiteral("0:20 20:12 32:10"));
        QCOMPARE(ops::clipById(d, r.ids[0])->mediaId, QStringLiteral("n"));
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleOverwriteLongerAndSeveralSegments()
    {
        TimelineData d = timeline();
        put(d, kV1, 0, 10);
        put(d, kV1, 10, 10);
        cut::SourceGroups groups {
            segment(0, 8, QStringLiteral("a")), segment(8, 7, QStringLiteral("b"))};
        const cut::Result r = cut::rippleOverwrite(d, groups, 2);
        QVERIFY(r.ok());
        QCOMPARE(layout(d, kV1), QStringLiteral("0:8 8:7 15:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:8 8:7"));
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleOverwriteNeedsAClip()
    {
        TimelineData d = timeline();
        put(d, kV1, 0, 10);
        const TimelineData before = d;
        QVERIFY(!cut::rippleOverwrite(d, {segment(0, 5, QStringLiteral("n"))}, 30).ok());
        QCOMPARE(d, before);
    }

    // ---- 35.9 place on top / 35.10 close up ----
    void placeOnTopUsesFirstFreeTrackForAllSegments()
    {
        TimelineData d = timeline();
        put(d, kV1, 0, 100);
        put(d, kV2, 30, 5); // V2 is busy inside [20, 45): a new V3 is used for both segments
        put(d, kA1, 0, 100);
        cut::SourceGroups groups {
            segment(0, 10, QStringLiteral("a")), segment(10, 15, QStringLiteral("b"))};
        const cut::Result r = cut::placeOnTop(d, groups, 20);
        QVERIFY(r.ok());
        QCOMPARE(int(d.videoTracks.size()), 3);
        QCOMPARE(layout(d, TrackRef {TrackType::Video, 2}), QStringLiteral("20:10 30:15"));
        QCOMPARE(layout(d, kA2), QStringLiteral("20:10 30:15")); // A2 was free
        QCOMPARE(layout(d, kV1), QStringLiteral("0:100")); // nothing overwritten
        QCOMPARE(r.end, int64_t(45));
        KK_VERIFY_INVARIANTS(d);
    }

    void closeUpIsZoomedVideoOnTop()
    {
        TimelineData d = timeline();
        put(d, kV1, 0, 100);
        put(d, kA1, 0, 100);
        const cut::Result r = cut::closeUp(d, {segment(0, 10, QStringLiteral("c"))}, 40);
        QVERIFY(r.ok());
        QCOMPARE(r.ids.size(), 1); // video only: the sound below continues
        QCOMPARE(layout(d, kV2), QStringLiteral("40:10"));
        QCOMPARE(layout(d, kA2), QString());
        const ClipData* c = ops::clipById(d, r.ids[0]);
        QCOMPARE(c->transform.value(keys::ZoomX, 0), cut::kCloseUpZoom);
        QCOMPARE(c->transform.value(keys::ZoomY, 0), cut::kCloseUpZoom);
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- 35.11 source overwrite ----
    void sourceOverwriteLinesUpByTimecode()
    {
        TimelineData d = timeline();
        // Timeline clip of "cam1" (timecode starts at 10 s) shows its source from 2 s.
        ClipData base = mediaClip(0, 200, 50, 1000, QStringLiteral("cam1"));
        base.mediaId = QStringLiteral("cam1");
        ops::overwriteClip(d, kV1, base);
        const cut::TimecodeLookup tc = [](const QString& id) -> std::optional<int64_t> {
            if (id == QLatin1String("cam1"))
                return 10'000'000; // 10 s
            if (id == QLatin1String("cam2"))
                return 11'000'000; // 11 s: cam2's source 0 = cam1's source 1 s
            return std::nullopt;
        };
        // Playhead at frame 25: cam1 source 3 s = 13 s absolute = cam2 source 2 s. Source in
        // point 1 s: the in point lands 1 s (25 frames) before the playhead.
        const auto start
            = cut::sourceOverwriteStart(d, 25, QStringLiteral("cam2"), 1'000'000, 5'000'000, tc);
        QVERIFY(start.has_value());
        QCOMPARE(*start, int64_t(0));
        // Outside the marked source range: refused.
        QVERIFY(!cut::sourceOverwriteStart(d, 25, QStringLiteral("cam2"), 2'500'000, 5'000'000, tc)
                     .has_value());
        // No timecode: refused.
        QVERIFY(!cut::sourceOverwriteStart(d, 25, QStringLiteral("none"), 0, 0, tc).has_value());

        ClipData src = mediaClip(0, 100, 25, 1000, QStringLiteral("cam2"));
        src.mediaId = QStringLiteral("cam2");
        const cut::Result r = cut::sourceOverwrite(d, {{{kV1, src}, {kA1, src}}}, *start);
        QVERIFY(r.ok());
        QCOMPARE(r.ids.size(), 1); // video only
        QCOMPARE(layout(d, kV2), QStringLiteral("0:100"));
        KK_VERIFY_INVARIANTS(d);
    }

    void sourceOverwriteCutsBeforeFrameZero()
    {
        TimelineData d = timeline();
        ClipData src = mediaClip(0, 50, 0, 1000, QStringLiteral("s"));
        const cut::Result r = cut::sourceOverwrite(d, {{{kV1, src}}}, -10);
        QVERIFY(r.ok());
        QCOMPARE(layout(d, kV1), QStringLiteral("0:40")); // V1 is free here
        QCOMPARE(ops::clipById(d, r.ids[0])->sourceInUs, 10 * kUsPerFrame25);
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- 35.14 fast review ----
    void fastReviewRateFollowsClipLength()
    {
        const Rational fps {25, 1};
        QCOMPARE(cut::fastReviewRate(25, fps), 1.0); // short clips: real time
        QCOMPARE(cut::fastReviewRate(100, fps), 2.0); // 4 s -> 2 s
        QCOMPARE(cut::fastReviewRate(250, fps), 5.0);
        QCOMPARE(cut::fastReviewRate(25 * 600, fps), cut::kFastReviewMaxRate);
        QVERIFY(cut::fastReviewRate(100, fps) < cut::fastReviewRate(200, fps));
    }
};

QTEST_GUILESS_MAIN(TestCutEdits)
#include "test_cutedits.moc"
