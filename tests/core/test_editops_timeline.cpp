// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the EditOps added by the timeline module: roll, slide, gapAt/closeGap,
// placeOnTop, copyClips/pasteClips, plus a random-operation invariant run covering all of them.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/ParamKeys.h"

#include <QRandomGenerator>
#include <QTest>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

class TestEditOpsTimeline : public QObject
{
    Q_OBJECT
private slots:
    // ---- roll ----
    void rollMovesCutBothWays()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 50, 0, 200));
        const quint64 b = ops::overwriteClip(d, kV1, mediaClip(50, 50, 20, 200));
        QCOMPARE(ops::rollEdit(d, a, b, 10), int64_t(10));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:60 60:40"));
        QCOMPARE(ops::clipById(d, b)->sourceInUs, 30 * kUsPerFrame25);
        QCOMPARE(ops::rollEdit(d, a, b, -25), int64_t(-25));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:35 35:65"));
        QCOMPARE(ops::clipById(d, b)->sourceInUs, 5 * kUsPerFrame25);
        KK_VERIFY_INVARIANTS(d);
    }

    void rollClampedByMediaAndDuration()
    {
        TimelineData d = timeline();
        // a uses source 0..50 of 60 frames: 10 frames of tail handle.
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 50, 0, 60));
        // b starts 5 frames into its source: 5 frames of head handle.
        const quint64 b = ops::overwriteClip(d, kV1, mediaClip(50, 20, 5, 100));
        QCOMPARE(ops::rollEdit(d, a, b, 100), int64_t(10));
        QCOMPARE(ops::rollEdit(d, a, b, -100), int64_t(-15)); // back 10, then 5 of b's handle
        QCOMPARE(layout(d, kV1), QStringLiteral("0:45 45:25"));
        // Unbounded media: limited by the shorter clip's duration.
        TimelineData e = timeline();
        const quint64 c = ops::overwriteClip(e, kV1, mediaClip(0, 10));
        const quint64 f = ops::overwriteClip(e, kV1, mediaClip(10, 4));
        QCOMPARE(ops::rollEdit(e, c, f, 100), int64_t(3));
        QCOMPARE(layout(e, kV1), QStringLiteral("0:13 13:1"));
        KK_VERIFY_INVARIANTS(d);
        KK_VERIFY_INVARIANTS(e);
    }

    void rollRequiresAdjacentUnlocked()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const quint64 b = ops::overwriteClip(d, kV1, mediaClip(12, 10));
        const quint64 c = ops::overwriteClip(d, kV2, mediaClip(10, 10));
        QCOMPARE(ops::rollEdit(d, a, b, 1), int64_t(0)); // gap between them
        QCOMPARE(ops::rollEdit(d, a, c, 1), int64_t(0)); // different tracks
        QCOMPARE(ops::rollEdit(d, b, a, 1), int64_t(0)); // wrong order
        ops::trimTail(d, a, 2, false);
        d.videoTracks[0].locked = true;
        QCOMPARE(ops::rollEdit(d, a, b, 1), int64_t(0));
    }

    // ---- slide ----
    void slideBetweenTouchingNeighbours()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 30, 0, 100));
        const quint64 m = ops::overwriteClip(d, kV1, mediaClip(30, 20, 0, 100));
        ops::overwriteClip(d, kV1, mediaClip(50, 30, 10, 100));
        const int64_t sourceBefore = ops::clipById(d, m)->sourceInUs;
        QCOMPARE(ops::slideClip(d, m, 5), int64_t(5));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:35 35:20 55:25"));
        QCOMPARE(ops::clipById(d, m)->sourceInUs, sourceBefore); // content unchanged
        QCOMPARE(d.videoTracks[0].clips[2].sourceInUs, 15 * kUsPerFrame25);
        // Left: the right neighbour's head handle is 15 frames now.
        QCOMPARE(ops::slideClip(d, m, -100), int64_t(-15));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:20 20:20 40:40"));
        KK_VERIFY_INVARIANTS(d);
    }

    void slideIntoGaps()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const quint64 m = ops::overwriteClip(d, kV1, mediaClip(15, 10));
        ops::overwriteClip(d, kV1, mediaClip(30, 10));
        QCOMPARE(ops::slideClip(d, m, 100), int64_t(5)); // gap of 5 on the right
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 20:10 30:10"));
        // Now touching the right neighbour, whose head (unbounded media) follows the slide.
        QCOMPARE(ops::slideClip(d, m, -100), int64_t(-10));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:10 20:20"));
        // Lone clip: limited by frame 0 only.
        TimelineData e = timeline();
        const quint64 lone = ops::overwriteClip(e, kV1, mediaClip(8, 10));
        QCOMPARE(ops::slideClip(e, lone, -20), int64_t(-8));
        QCOMPARE(ops::slideClip(e, lone, 40), int64_t(40));
        KK_VERIFY_INVARIANTS(d);
        KK_VERIFY_INVARIANTS(e);
    }

    // ---- gaps ----
    void gapAtFindsEmptyRange()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV1, mediaClip(30, 10));
        QCOMPARE(
            ops::gapAt(d, kV1, 5), std::make_optional(std::make_pair<int64_t, int64_t>(0, 10)));
        QCOMPARE(
            ops::gapAt(d, kV1, 25), std::make_optional(std::make_pair<int64_t, int64_t>(20, 30)));
        QCOMPARE(
            ops::gapAt(d, kV1, 20), std::make_optional(std::make_pair<int64_t, int64_t>(20, 30)));
        QVERIFY(!ops::gapAt(d, kV1, 15)); // inside a clip
        QVERIFY(!ops::gapAt(d, kV1, 45)); // after the last clip
        QVERIFY(!ops::gapAt(d, kV2, 0)); // empty track
    }

    void closeGapTargetTrack()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(25, 10));
        ops::overwriteClip(d, kV1, mediaClip(40, 10));
        ops::overwriteClip(d, kA1, mediaClip(30, 10));
        QCOMPARE(ops::closeGap(d, kV1, 12), int64_t(15));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:10 25:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("30:10")); // untouched
        QCOMPARE(ops::closeGap(d, kV1, 5), int64_t(0)); // no gap
        KK_VERIFY_INVARIANTS(d);
    }

    void closeGapAllTracksKeepsSync()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(30, 10));
        ops::overwriteClip(d, kA1, mediaClip(0, 18)); // only 12 frames free left of 30
        ops::overwriteClip(d, kA1, mediaClip(30, 10));
        QCOMPARE(ops::closeGap(d, kV1, 15, ops::RippleScope::AllTracks), int64_t(12));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 18:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:18 18:10"));
        // A clip straddling the cut on another track no longer blocks the close (spec 10.15,
        // which supersedes 6.4 here): V1 closes by the full gap, the straddling track stays.
        TimelineData e = timeline();
        ops::overwriteClip(e, kV1, mediaClip(0, 10));
        ops::overwriteClip(e, kV1, mediaClip(30, 10));
        ops::overwriteClip(e, kV2, mediaClip(20, 20));
        QCOMPARE(ops::closeGap(e, kV1, 15, ops::RippleScope::AllTracks), int64_t(20));
        QCOMPARE(layout(e, kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(e, kV2), QStringLiteral("20:20")); // blocking track left alone
        TimelineData f = timeline();
        ops::overwriteClip(f, kV1, mediaClip(0, 10));
        ops::overwriteClip(f, kV1, mediaClip(30, 10));
        ops::overwriteClip(f, kV2, mediaClip(20, 20));
        f.videoTracks[1].locked = true; // locked tracks are ignored (and not shifted)
        QCOMPARE(ops::closeGap(f, kV1, 15, ops::RippleScope::AllTracks), int64_t(20));
        QCOMPARE(layout(f, kV2), QStringLiteral("20:20"));
        KK_VERIFY_INVARIANTS(f);
        KK_VERIFY_INVARIANTS(d);
        KK_VERIFY_INVARIANTS(e);
    }

    void closeGapBlockedTrackIsLeftAlone()
    {
        // Gap on V1, a long clip on A1 across it, free room on A2: V1 and A2 close, A1 stays.
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(30, 10));
        ops::overwriteClip(d, kA1, mediaClip(0, 50));
        ops::overwriteClip(d, kA2, mediaClip(0, 10));
        ops::overwriteClip(d, kA2, mediaClip(30, 10));
        QCOMPARE(ops::closeGap(d, kV1, 15, ops::RippleScope::AllTracks), int64_t(20));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(d, kA2), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:50"));
        KK_VERIFY_INVARIANTS(d);
    }

    void closeGapLinkedPartnerTrackMovesToo()
    {
        // V1's second clip is linked to an A1 clip that would stay (A1 is not auto-select):
        // A1 has room, so it moves with V1 and the pair stays in sync (spec 10.16).
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        Q_UNUSED(v);
        const quint64 v2 = ops::overwriteClip(d, kV1, mediaClip(30, 10));
        const quint64 a = ops::overwriteClip(d, kA1, mediaClip(30, 10));
        ops::linkClips(d, {v2, a});
        d.audioTracks[0].autoSelect = false;
        bool broke = true;
        QCOMPARE(ops::closeGap(d, kV1, 15, ops::RippleScope::AllTracks, &broke), int64_t(20));
        QVERIFY(!broke);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("10:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    void closeGapLinkedPartnerBlockedKeepsClickedTrackStill()
    {
        // The partner starts before the cut and runs across it: its track cannot move, so V1
        // must not move either. An unlinked track with room is not held back by it.
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const quint64 v2 = ops::overwriteClip(d, kV1, mediaClip(30, 10));
        const quint64 a = ops::overwriteClip(d, kA1, mediaClip(25, 15));
        ops::linkClips(d, {v2, a});
        bool broke = false;
        QCOMPARE(ops::closeGap(d, kV1, 15, ops::RippleScope::AllTracks, &broke), int64_t(0));
        QVERIFY(broke);
        QVERIFY(!ops::canCloseGap(d, kV1, 15, ops::RippleScope::AllTracks));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 30:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("25:15"));
        // Another track whose clips have no links still closes when the clicked track is free.
        TimelineData e = timeline();
        ops::overwriteClip(e, kV1, mediaClip(0, 10));
        ops::overwriteClip(e, kV1, mediaClip(30, 10));
        const quint64 w = ops::overwriteClip(e, kV2, mediaClip(30, 10));
        const quint64 b = ops::overwriteClip(e, kA1, mediaClip(25, 15));
        ops::linkClips(e, {w, b});
        ops::overwriteClip(e, kA2, mediaClip(0, 10));
        ops::overwriteClip(e, kA2, mediaClip(30, 10));
        QCOMPARE(ops::closeGap(e, kV1, 15, ops::RippleScope::AllTracks, &broke), int64_t(20));
        QVERIFY(!broke);
        QCOMPARE(layout(e, kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(e, kV2), QStringLiteral("30:10")); // linked to the blocked A1: stays
        QCOMPARE(layout(e, kA2), QStringLiteral("0:10 10:10"));
        KK_VERIFY_INVARIANTS(d);
        KK_VERIFY_INVARIANTS(e);
    }

    void canCloseGapOnlyFalseWithoutGapOrOnLockedTrack()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(30, 10));
        const auto all = ops::RippleScope::AllTracks;
        QVERIFY(ops::canCloseGap(d, kV1, 15, all));
        QVERIFY(!ops::canCloseGap(d, kV1, 5, all)); // on a clip
        QVERIFY(!ops::canCloseGap(d, kV1, 60, all)); // after the last clip: open end
        ops::overwriteClip(d, kV2, mediaClip(20, 20)); // straddles the cut: does not matter
        QVERIFY(ops::canCloseGap(d, kV1, 15, all));
        d.videoTracks[0].locked = true;
        QVERIFY(!ops::canCloseGap(d, kV1, 15, all));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 30:10")); // a dry run changes nothing
    }

    // ---- place on top ----
    void placeOnTopFindsFreeTrack()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 100));
        ops::overwriteClip(d, kV2, mediaClip(40, 10));
        // Free on V2 at 0..20 -> V2.
        const auto first = ops::placeOnTop(d, {ops::PlacedClip {kV1, mediaClip(0, 20)}});
        QCOMPARE(first.size(), 1);
        QCOMPARE(ops::findClip(d, first[0])->track, kV2);
        // 35..55 collides on V1 and V2 -> new V3.
        const auto second = ops::placeOnTop(d, {ops::PlacedClip {kV1, mediaClip(35, 20)}});
        QCOMPARE(d.videoTracks.size(), size_t(3));
        QCOMPARE(ops::findClip(d, second[0])->track, (TrackRef {TrackType::Video, 2}));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:100")); // nothing overwritten
        KK_VERIFY_INVARIANTS(d);
    }

    void placeOnTopSkipsLockedAndRejectsInvalid()
    {
        TimelineData d = timeline();
        d.videoTracks[1].locked = true;
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const auto ids = ops::placeOnTop(
            d, {ops::PlacedClip {kV1, mediaClip(0, 5)}, ops::PlacedClip {kA1, mediaClip(0, 5)}});
        QCOMPARE(ids.size(), 2);
        QCOMPARE(ops::findClip(d, ids[0])->track, (TrackRef {TrackType::Video, 2}));
        QCOMPARE(ops::findClip(d, ids[1])->track, kA1);
        QVERIFY(ops::placeOnTop(d, {ops::PlacedClip {kV1, mediaClip(0, 0)}}).isEmpty());
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- clipboard ----
    void copyPasteKeepsLayoutWithFreshIds()
    {
        TimelineData d = timeline();
        ClipData v = mediaClip(10, 20);
        v.effects.append(EffectInstance {d.allocateId(), QStringLiteral("gaussian_blur")});
        const quint64 vid = ops::overwriteClip(d, kV2, v);
        const quint64 aid = ops::overwriteClip(d, kA1, mediaClip(10, 20));
        const quint64 other = ops::overwriteClip(d, kA2, mediaClip(15, 5));
        ops::linkClips(d, {vid, aid});
        const ops::Clipboard board = ops::copyClips(d, {vid, aid, other});
        QCOMPARE(board.clips.size(), 3);
        QCOMPARE(board.duration, int64_t(20));
        QCOMPARE(board.baseVideoTrack, 1);

        const auto ids = ops::pasteClips(d, board, 100);
        QCOMPARE(ids.size(), 3);
        QCOMPARE(layout(d, kV2), QStringLiteral("10:20 100:20"));
        QCOMPARE(layout(d, kA1), QStringLiteral("10:20 100:20"));
        QCOMPARE(layout(d, kA2), QStringLiteral("15:5 105:5"));
        const ClipData* pv = &d.videoTracks[1].clips[1];
        const ClipData* pa = &d.audioTracks[0].clips[1];
        QVERIFY(pv->linkGroup != 0);
        QCOMPARE(pv->linkGroup, pa->linkGroup);
        QVERIFY(pv->linkGroup != ops::clipById(d, vid)->linkGroup);
        QVERIFY(pv->effects.front().id != v.effects.front().id);
        QCOMPARE(d.audioTracks[1].clips[1].linkGroup, quint64(0));
        KK_VERIFY_INVARIANTS(d);
    }

    void pasteToOtherTracksAddsMissingAndInserts()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV2, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(0, 50));
        const ops::Clipboard board = ops::copyClips(d, {v});
        const auto ids = ops::pasteClips(d, board, 20, 3, -1);
        QCOMPARE(ids.size(), 1);
        QCOMPARE(d.videoTracks.size(), size_t(4));
        QCOMPARE(layout(d, TrackRef {TrackType::Video, 3}), QStringLiteral("20:10"));
        // Insert mode ripples V1.
        ops::pasteClips(d, board, 20, 0, -1, true);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:20 20:10 30:30"));
        // Locked destination: nothing changes.
        d.videoTracks[0].locked = true;
        const TimelineData before = d;
        QVERIFY(ops::pasteClips(d, board, 0, 0, -1).isEmpty());
        QCOMPARE(d, before);
        KK_VERIFY_INVARIANTS(d);
    }

    /// Random sequences including the new operations must never break the invariants.
    void randomOperationsKeepInvariants()
    {
        QRandomGenerator rng(4321);
        TimelineData d = timeline();
        ops::Clipboard board;
        auto anyClip = [&]() -> quint64 {
            QVector<quint64> ids;
            for (const auto* list : {&d.videoTracks, &d.audioTracks})
                for (const TrackData& t : *list)
                    for (const ClipData& c : t.clips)
                        ids.append(c.id);
            return ids.isEmpty() ? 0 : ids[rng.bounded(int(ids.size()))];
        };
        auto anyTrack = [&]() {
            const TrackType type = rng.bounded(2) ? TrackType::Video : TrackType::Audio;
            return TrackRef {type, int(rng.bounded(int(d.tracks(type).size())))};
        };
        for (int step = 0; step < 3000; ++step) {
            const int64_t f = rng.bounded(300);
            const int64_t len = 1 + rng.bounded(40);
            switch (rng.bounded(9)) {
            case 0:
                ops::overwriteClip(d, anyTrack(), mediaClip(f, len, rng.bounded(50), 200));
                break;
            case 1: {
                const quint64 id = anyClip();
                if (const auto loc = ops::findClip(d, id)) {
                    const auto& clips = d.track(loc->track)->clips;
                    if (size_t(loc->clipIndex) + 1 < clips.size())
                        ops::rollEdit(
                            d, id, clips[size_t(loc->clipIndex) + 1].id, rng.bounded(-20, 20));
                }
                break;
            }
            case 2:
                ops::slideClip(d, anyClip(), rng.bounded(-20, 20));
                break;
            case 3:
                ops::closeGap(d, anyTrack(), f,
                    rng.bounded(2) ? ops::RippleScope::AllTracks : ops::RippleScope::TargetTracks);
                break;
            case 4:
                if (d.videoTracks.size() < 8)
                    ops::placeOnTop(d, {ops::PlacedClip {anyTrack(), mediaClip(f, len)}});
                break;
            case 5:
                board = ops::copyClips(d, {anyClip(), anyClip(), anyClip()});
                break;
            case 6:
                if (d.videoTracks.size() < 8 && d.audioTracks.size() < 8)
                    ops::pasteClips(d, board, f, -1, -1, rng.bounded(2));
                break;
            case 7:
                ops::liftClips(d, {anyClip()});
                break;
            case 8:
                ops::moveClips(d, {anyClip()}, rng.bounded(-30, 30), rng.bounded(-1, 2));
                break;
            }
            QString why;
            if (!ops::checkInvariants(d, &why))
                QFAIL(qPrintable(QStringLiteral("step %1: %2").arg(step).arg(why)));
        }
    }
};

QTEST_GUILESS_MAIN(TestEditOpsTimeline)
#include "test_editops_timeline.moc"
