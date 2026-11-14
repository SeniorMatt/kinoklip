// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the ripple / range EditOps (EditOpsRipple.cpp): ripple delete across tracks,
// lift / extract / copy of in-out ranges, delete gaps, join, swap, shuffle, ripple overwrite.
// Spec item numbers refer to docs/behavior-spec.md.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/ParamKeys.h"

#include <QRandomGenerator>
#include <QTest>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

class TestEditOpsRipple : public QObject
{
    Q_OBJECT
private slots:
    // ---- ripple delete (10.2, 10.4) ----
    void rippleDeleteTargetTracksOnly()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV2, mediaClip(30, 10));
        QCOMPARE(ops::rippleDeleteClips(d, {a}), 1);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10"));
        QCOMPARE(layout(d, kV2), QStringLiteral("30:10")); // TargetTracks: V2 untouched
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleDeleteAllTracksKeepsSyncWhereThereIsRoom()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV1, mediaClip(20, 10));
        ops::overwriteClip(d, kV2, mediaClip(25, 10)); // later, room in [10,20): follows
        ops::overwriteClip(d, kA1, mediaClip(5, 20)); // spans the range: stays
        ops::overwriteClip(d, kA1, mediaClip(40, 5)); // after a non-free range: stays
        ops::overwriteClip(d, kA2, mediaClip(30, 5));
        d.audioTracks[1].syncLock = false; // sync lock off: stays
        QCOMPARE(ops::rippleDeleteClips(d, {a}, ops::RippleScope::AllTracks), 1);
        QCOMPARE(layout(d, kV1), QStringLiteral("10:10"));
        QCOMPARE(layout(d, kV2), QStringLiteral("15:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("5:20 40:5"));
        QCOMPARE(layout(d, kA2), QStringLiteral("30:5"));
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleDeleteSeveralRangesAndLockedTracks()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV1, mediaClip(20, 10));
        const quint64 c = ops::overwriteClip(d, kV1, mediaClip(30, 10));
        ops::overwriteClip(d, kV1, mediaClip(40, 10));
        ops::overwriteClip(d, kV2, mediaClip(20, 10));
        ops::overwriteClip(d, kV2, mediaClip(45, 5));
        ops::overwriteClip(d, kA1, mediaClip(45, 5));
        d.audioTracks[0].locked = true;
        QCOMPARE(ops::rippleDeleteClips(d, {a, c}, ops::RippleScope::AllTracks), 2);
        QCOMPARE(layout(d, kV1), QStringLiteral("10:10 20:10"));
        QCOMPARE(layout(d, kV2), QStringLiteral("10:10 25:5"));
        QCOMPARE(layout(d, kA1), QStringLiteral("45:5")); // locked
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleDeleteLinkedPairClosesBothTracks()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const quint64 au = ops::overwriteClip(d, kA1, mediaClip(0, 10));
        ops::linkClips(d, {v, au});
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kA1, mediaClip(10, 10));
        QCOMPARE(ops::rippleDeleteClips(d, ops::linkedClips(d, v), ops::RippleScope::AllTracks), 2);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:10"));
    }

    // ---- ranges (10.7, 10.8, 16.5) ----
    void liftRangeLeavesGap()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 30, 0, 100));
        ops::overwriteClip(d, kV2, mediaClip(0, 30));
        d.videoTracks[1].autoSelect = false;
        QVERIFY(ops::liftRange(d, 10, 20, ops::autoSelectTracks()));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 20:10"));
        QCOMPARE(layout(d, kV2), QStringLiteral("0:30"));
        QCOMPARE(d.videoTracks[0].clips[1].sourceInUs, 20 * kUsPerFrame25);
        QVERIFY(!ops::liftRange(d, 10, 20, ops::autoSelectTracks())); // already empty
        KK_VERIFY_INVARIANTS(d);
    }

    void extractRangeRipples()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 30, 0, 100));
        ops::overwriteClip(d, kV1, mediaClip(40, 10));
        ops::overwriteClip(d, kA1, mediaClip(25, 10));
        ops::overwriteClip(d, kA2, mediaClip(0, 50));
        d.audioTracks[1].locked = true;
        QVERIFY(ops::extractRange(d, 10, 20, ops::autoSelectTracks()));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:10 30:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("15:10"));
        QCOMPARE(layout(d, kA2), QStringLiteral("0:50"));
        KK_VERIFY_INVARIANTS(d);
        // Nothing inside the range and nothing after it: no change.
        QVERIFY(ops::extractRange(d, 40, 45, ops::autoSelectTracks()) == false);
        QVERIFY(!ops::extractRange(d, 20, 10));
    }

    void copyRangeKeepsLeadingRoom()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 20, 0, 100));
        ops::overwriteClip(d, kV2, mediaClip(15, 20));
        const ops::Clipboard board = ops::copyRange(d, 10, 30);
        QCOMPARE(board.duration, int64_t(20));
        QCOMPARE(board.clips.size(), 2);
        int64_t starts[2] = {-1, -1};
        for (const ops::ClipboardClip& c : board.clips)
            starts[c.type == TrackType::Video && c.trackOffset == 0 ? 0 : 1] = c.clip.start;
        QCOMPARE(starts[0], int64_t(0)); // V1 part 10..20
        QCOMPARE(starts[1], int64_t(5)); // V2 part 15..30 starts 5 into the range
        // The source data is not changed.
        QCOMPARE(layout(d, kV1), QStringLiteral("0:20"));
    }

    void deleteGapsKeepsSync()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(5, 10));
        ops::overwriteClip(d, kV1, mediaClip(30, 10));
        ops::overwriteClip(d, kA1, mediaClip(20, 5)); // V1 gap 15..30 is partly used on A1
        ops::overwriteClip(d, kA2, mediaClip(60, 5));
        d.audioTracks[1].autoSelect = false;
        // Common gaps: 0..5, 15..20, 25..30.
        QCOMPARE(ops::deleteGaps(d), int64_t(15));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 15:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("10:5"));
        QCOMPARE(layout(d, kA2), QStringLiteral("60:5"));
        QCOMPARE(ops::deleteGaps(d), int64_t(0));
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- join (through edits) ----
    void joinThroughEdit()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 40, 0, 100));
        ops::clipById(d, a)->transform.animated(keys::Opacity, 1.0).setKey(30, 0.5);
        const QVector<quint64> right = ops::splitClips(d, {a}, 20);
        QCOMPARE(right.size(), 1);
        QVERIFY(ops::canJoin(d, a, right[0]));
        QVERIFY(ops::joinClips(d, a, right[0]));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:40"));
        QVERIFY(ops::clipById(d, a)->transform.values.value(keys::Opacity).hasKeyAt(30));
        KK_VERIFY_INVARIANTS(d);
        // Not a through edit: different source position.
        const quint64 b = ops::overwriteClip(d, kV1, mediaClip(40, 10, 70, 100));
        QVERIFY(!ops::canJoin(d, a, b));
        QVERIFY(!ops::joinClips(d, a, b));
    }

    // ---- swap / shuffle (21.6) ----
    void swapWithNeighbourKeepsGap()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(10, 10, 0, 0, QStringLiteral("a")));
        const quint64 b = ops::overwriteClip(d, kV1, mediaClip(25, 5, 0, 0, QStringLiteral("b")));
        QCOMPARE(ops::swapWithNeighbour(d, a, +1), b);
        QCOMPARE(layout(d, kV1), QStringLiteral("10:5 20:10"));
        QCOMPARE(d.videoTracks[0].clips[0].id, b);
        QCOMPARE(ops::swapWithNeighbour(d, a, -1), b);
        QCOMPARE(layout(d, kV1), QStringLiteral("10:10 25:5"));
        QCOMPARE(ops::swapWithNeighbour(d, a, -1), quint64(0));
        KK_VERIFY_INVARIANTS(d);
    }

    void shuffleMovesAndCloses()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 20));
        ops::overwriteClip(d, kV1, mediaClip(30, 5));
        // Take a out (b, c move to 0, 20), insert it at 20 (between b and c).
        QVERIFY(ops::shuffleClip(d, a, 20));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:20 20:10 30:5"));
        QCOMPARE(d.videoTracks[0].clips[1].id, a);
        QVERIFY(!ops::shuffleClip(d, a, 20));
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- ripple overwrite (4.11) ----
    void rippleOverwriteLongerAndShorter()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV1, mediaClip(20, 10));
        ops::overwriteClip(d, kA1, mediaClip(25, 10));
        // Replace 10..20 with 15 frames: later clips move 5 right on V1 and A1.
        const QVector<quint64> ids
            = ops::rippleOverwriteClips(d, {ops::PlacedClip {kV1, mediaClip(0, 15)}}, 10, 20);
        QCOMPARE(ids.size(), 1);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:15 25:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("30:10"));
        // Replace it again with 5 frames: everything moves 10 back (A1 has room).
        ops::rippleOverwriteClips(d, {ops::PlacedClip {kV1, mediaClip(0, 5)}}, 10, 25);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:5 15:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("20:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    void randomRippleOperationsKeepInvariants()
    {
        QRandomGenerator rng(4321);
        TimelineData d = timeline();
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
            switch (rng.bounded(10)) {
            case 0:
            case 1:
                ops::overwriteClip(d, anyTrack(), mediaClip(f, len, rng.bounded(50), 200));
                break;
            case 2:
                ops::rippleDeleteClips(d, {anyClip(), anyClip()}, ops::RippleScope::AllTracks);
                break;
            case 3:
                ops::extractRange(d, f, f + len, ops::autoSelectTracks());
                break;
            case 4:
                ops::liftRange(d, f, f + len);
                break;
            case 5:
                ops::deleteGaps(d);
                break;
            case 6:
                ops::swapWithNeighbour(d, anyClip(), rng.bounded(2) ? 1 : -1);
                break;
            case 7:
                ops::shuffleClip(d, anyClip(), f);
                break;
            case 8:
                ops::rippleOverwriteClips(
                    d, {ops::PlacedClip {anyTrack(), mediaClip(0, len)}}, f, f + rng.bounded(30));
                break;
            case 9:
                ops::splitAt(d, f);
                d.videoTracks[0].locked = rng.bounded(4) == 0;
                break;
            }
            QString why;
            if (!ops::checkInvariants(d, &why))
                QFAIL(qPrintable(QStringLiteral("step %1: %2").arg(step).arg(why)));
        }
    }

    // ---- track sets: the ranges of the Vim operators (spec 83.25) ----
    void trackSetFilterPicksTheListedTracks()
    {
        TimelineData d = timeline();
        const ops::TrackFilter filter = ops::trackSetFilter({kV1, kA1});
        QVERIFY(filter(kV1, d.videoTracks[0]));
        QVERIFY(filter(kA1, d.audioTracks[0]));
        QVERIFY(!filter(kV2, d.videoTracks[1]));
        QVERIFY(!filter(kA2, d.audioTracks[1]));
    }

    void extractOnATrackSetRipplesOnlyThoseTracks()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 40));
        ops::overwriteClip(d, kV1, mediaClip(40, 40));
        ops::overwriteClip(d, kV2, mediaClip(0, 40));
        ops::overwriteClip(d, kV2, mediaClip(40, 40));
        QVERIFY(ops::extractRange(d, 10, 30, ops::trackSetFilter({kV1})));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:10 20:40")); // the cut clip is two pieces
        QCOMPARE(layout(d, kV2), QStringLiteral("0:40 40:40")); // not in the set
        KK_VERIFY_INVARIANTS(d);
    }

    void liftOnATrackSetLeavesTheGap()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 40));
        ops::overwriteClip(d, kV1, mediaClip(40, 40));
        ops::overwriteClip(d, kA1, mediaClip(0, 80));
        QVERIFY(ops::liftRange(d, 10, 50, ops::trackSetFilter({kV1})));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 50:30"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:80"));
        KK_VERIFY_INVARIANTS(d);
    }

    void linkedPartnersJoinTheTrackSet()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 40));
        const quint64 a = ops::overwriteClip(d, kA1, mediaClip(0, 40));
        ops::linkClips(d, {v, a});
        ops::overwriteClip(d, kV2, mediaClip(0, 40)); // not linked
        // V1 alone gets A1 with it, and only where a linked clip touches the range
        const auto tracks = ops::tracksWithLinkedPartners(d, {kV1}, 10, 20);
        QCOMPARE(tracks, (std::vector<TrackRef> {kV1, kA1}));
        QCOMPARE(ops::tracksWithLinkedPartners(d, {kV1}, 50, 60), (std::vector<TrackRef> {kV1}));
        QCOMPARE(ops::tracksWithLinkedPartners(d, {kV2}, 10, 20), (std::vector<TrackRef> {kV2}));
        // an unknown track is kept as given
        QVERIFY(ops::extractRange(d, 10, 30, ops::trackSetFilter(tracks)));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(d, kV2), QStringLiteral("0:40"));
        // the pieces of the cut clips stay linked to their partners
        QCOMPARE(d.videoTracks[0].clips[0].linkGroup, d.audioTracks[0].clips[0].linkGroup);
        QCOMPARE(d.videoTracks[0].clips[1].linkGroup, d.audioTracks[0].clips[1].linkGroup);
        QVERIFY(d.videoTracks[0].clips[0].linkGroup != d.videoTracks[0].clips[1].linkGroup);
        KK_VERIFY_INVARIANTS(d);
    }

    void partnersWithDifferentSpansLoseTheSameFrames()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 40));
        const quint64 a = ops::overwriteClip(d, kA1, mediaClip(0, 30));
        ops::linkClips(d, {v, a});
        const auto tracks = ops::tracksWithLinkedPartners(d, {kV1}, 20, 35);
        QVERIFY(ops::extractRange(d, 20, 35, ops::trackSetFilter(tracks)));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:20 20:5"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:20"));
        KK_VERIFY_INVARIANTS(d);
    }

    void lockedTracksOfASetAreNotChanged()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 40));
        ops::overwriteClip(d, kA1, mediaClip(0, 40));
        d.audioTracks[0].locked = true;
        QVERIFY(ops::extractRange(d, 10, 20, ops::trackSetFilter({kV1, kA1})));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:20"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:40")); // locked: never touched
        d.videoTracks[0].locked = true;
        QVERIFY(!ops::extractRange(d, 10, 20, ops::trackSetFilter({kV1, kA1})));
        QVERIFY(!ops::liftRange(d, 10, 20, ops::trackSetFilter({kV1, kA1})));
        // copying from a locked track is fine
        QVERIFY(!ops::copyRange(d, 10, 20, ops::trackSetFilter({kV1})).isEmpty());
    }

    void copyRangeOfATrackSetCutsTheEdges()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 40));
        ops::overwriteClip(d, kV2, mediaClip(0, 40));
        const ops::Clipboard board = ops::copyRange(d, 10, 25, ops::trackSetFilter({kV1}));
        QCOMPARE(board.clips.size(), qsizetype(1));
        QCOMPARE(board.clips[0].clip.duration, int64_t(15));
        QCOMPARE(board.duration, int64_t(15));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:40")); // copying changes nothing
    }

    // ---- ripple trim keeps the other sync-locked tracks in sync (8.18, 97) ----
    void rippleTrimTailLengtheningCutsSpanningClipsAndOpensGaps()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV2, mediaClip(5, 20)); // spans the edit point: cut there
        ops::overwriteClip(d, kV2, mediaClip(30, 5));
        ops::overwriteClip(d, kA1, mediaClip(12, 8));
        ops::overwriteClip(d, kA2, mediaClip(14, 4));
        d.audioTracks[1].syncLock = false; // sync lock off: stays
        QCOMPARE(ops::rippleTrimSynced(d, {a}, false, 3), 3);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:13 13:10"));
        // Changed from "5:20 33:5": the spanning clip is no longer left alone, its tail follows.
        QCOMPARE(layout(d, kV2), QStringLiteral("5:5 13:15 33:5"));
        QCOMPARE(layout(d, kA1), QStringLiteral("15:8"));
        QCOMPARE(layout(d, kA2), QStringLiteral("14:4"));
        // The cut keeps the picture continuous: the second part shows later source material.
        QVERIFY(d.videoTracks[1].clips[1].sourceInUs > d.videoTracks[1].clips[0].sourceInUs);
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleTrimHeadAndLockedTracks()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kA1, mediaClip(10, 10));
        ops::overwriteClip(d, kA2, mediaClip(12, 4));
        d.audioTracks[1].locked = true; // locked: stays (even with sync lock on)
        QCOMPARE(ops::rippleTrimSynced(d, {a}, true, 4), 4); // head +4 shortens by 4
        QCOMPARE(layout(d, kV1), QStringLiteral("0:6 6:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("6:10"));
        QCOMPARE(layout(d, kA2), QStringLiteral("12:4"));
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleTrimShorteningRemovesTheSameFramesFromOtherTracks()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 20));
        ops::overwriteClip(d, kV1, mediaClip(20, 10));
        ops::overwriteClip(d, kV2, mediaClip(0, 12)); // ends at 12, next starts at 20
        ops::overwriteClip(d, kV2, mediaClip(20, 5));
        // Changed from "limited to 8 frames by the room on V2": nothing limits the trim now;
        // frames 5..20 leave V2 as well (its first clip is trimmed to 0:5).
        QCOMPARE(ops::rippleTrimSynced(d, {a}, false, -15), -15);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:5 5:10"));
        QCOMPARE(layout(d, kV2), QStringLiteral("0:5 5:5"));
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleTrimShorteningTrimsSpanningClipsAndDropsInnerOnes()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV2, mediaClip(5, 20)); // spans the point: loses frames 7..10
        ops::overwriteClip(d, kA1, mediaClip(7, 3)); // exactly inside 7..10: removed
        ops::overwriteClip(d, kA1, mediaClip(12, 4));
        QCOMPARE(ops::rippleTrimSynced(d, {a}, false, -3), -3);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:7 7:10"));
        QCOMPARE(layout(d, kV2), QStringLiteral("5:2 7:15"));
        QCOMPARE(layout(d, kA1), QStringLiteral("9:4"));
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleTrimWithSyncLockOffOnEveryOtherTrackMovesNothingElse()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV2, mediaClip(5, 20));
        ops::overwriteClip(d, kA1, mediaClip(12, 4));
        d.videoTracks[1].syncLock = false;
        d.audioTracks[0].syncLock = false;
        QCOMPARE(ops::rippleTrimSynced(d, {a}, false, 4), 4);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:14 14:10"));
        QCOMPARE(layout(d, kV2), QStringLiteral("5:20"));
        QCOMPARE(layout(d, kA1), QStringLiteral("12:4"));
        QCOMPARE(ops::rippleTrimSynced(d, {a}, false, -4), -4);
        QCOMPARE(layout(d, kV2), QStringLiteral("5:20"));
        QCOMPARE(layout(d, kA1), QStringLiteral("12:4"));
    }

    void autoSelectDoesNotDecideWhatRipples()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV2, mediaClip(12, 4));
        d.videoTracks[1].autoSelect = false; // Resolve 20.2: only sync lock counts
        QCOMPARE(ops::rippleTrimSynced(d, {a}, false, 2), 2);
        QCOMPARE(layout(d, kV2), QStringLiteral("14:4"));
    }

    void rippleTrimLinkedPartnersAreNotShiftedTwice()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const quint64 au = ops::overwriteClip(d, kA1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kA1, mediaClip(10, 10));
        ops::overwriteClip(d, kA2, mediaClip(10, 10));
        QCOMPARE(ops::rippleTrimSynced(d, {v, au}, false, 5), 5);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:15 15:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:15 15:10"));
        QCOMPARE(layout(d, kA2), QStringLiteral("15:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    // Owner report 1.8.1 (spec 97.7): a compound clip on a track above V3 follows a ripple trim.
    void rippleTrimMovesACompoundClipOnAHighTrack()
    {
        for (int mode = 0; mode < 4; ++mode) {
            TimelineData d = timeline();
            ops::addTrack(d, TrackType::Video);
            ops::addTrack(d, TrackType::Video);
            ops::addTrack(d, TrackType::Video);
            const TrackRef kV5 {TrackType::Video, 4};
            const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 100, 50, 1000));
            const quint64 au = ops::overwriteClip(d, kA1, mediaClip(0, 100, 50, 1000));
            ops::linkClips(d, {v, au});
            ops::overwriteClip(d, kV1, mediaClip(100, 100, 0, 1000));
            ops::overwriteClip(d, kV2, mediaClip(150, 20));
            ClipData comp = mediaClip(1500, 200);
            comp.kind = ClipKind::Nested;
            comp.nestedUid = 7;
            comp.mediaId.clear();
            ops::overwriteClip(d, kV5, comp);
            const bool head = mode & 1;
            const int64_t delta = mode & 2 ? -10 : 10; // tail: -10 shortens; head: +10 shortens
            const int64_t shift = head ? -delta : delta;
            const int64_t applied = ops::rippleTrimSynced(d, {v, au}, head, delta);
            QCOMPARE(applied, delta);
            QCOMPARE(layout(d, kV5), QStringLiteral("%1:200").arg(1500 + shift));
            QCOMPARE(layout(d, kV2), QStringLiteral("%1:20").arg(150 + shift));
            KK_VERIFY_INVARIANTS(d);
        }
    }

    void rippleTrimCutsAndMovesASpanningCompoundClip()
    {
        for (int mode = 0; mode < 2; ++mode) {
            TimelineData d = timeline();
            ops::addTrack(d, TrackType::Video);
            ops::addTrack(d, TrackType::Video);
            const TrackRef kV4 {TrackType::Video, 3};
            const quint64 v = ops::overwriteClip(d, kV1, mediaClip(100, 100, 50, 1000));
            ClipData comp = mediaClip(40, 300, 0, 300);
            comp.kind = ClipKind::Nested;
            comp.nestedUid = 7;
            comp.mediaId.clear();
            ops::overwriteClip(d, kV4, comp);
            const bool shorten = mode == 0;
            QCOMPARE(ops::rippleTrimSynced(d, {v}, false, shorten ? -10 : 10), shorten ? -10 : 10);
            // The compound clip is cut at the edit point (frame 200); the part after it moves.
            QCOMPARE(layout(d, kV4),
                shorten ? QStringLiteral("40:150 190:140") : QStringLiteral("40:160 210:140"));
            QCOMPARE(d.videoTracks[3].clips[1].kind, ClipKind::Nested);
            KK_VERIFY_INVARIANTS(d);
        }
    }

    // Owner report 1.8.1 ("effect id 314 is not unique"): the piece of a clip cut by a sync ripple
    // (or by clearing a range) gets fresh effect and mask ids, and a timeline saved with
    // duplicates is repaired by normalizeIds.
    void splitPiecesGetFreshEffectIds()
    {
        for (const int64_t delta : {10, -10}) {
            TimelineData d = timeline();
            const quint64 v = ops::overwriteClip(d, kV1, mediaClip(100, 100, 50, 1000));
            ClipData over = mediaClip(40, 300, 0, 300);
            EffectInstance fx;
            fx.id = d.allocateId();
            fx.type = QStringLiteral("blur");
            over.effects.append(fx);
            ops::overwriteClip(d, kV2, over);
            QCOMPARE(ops::rippleTrimSynced(d, {v}, false, delta), delta);
            QVERIFY(d.videoTracks[1].clips.size() >= 2);
            KK_VERIFY_INVARIANTS(d);
        }
        TimelineData d = timeline();
        ClipData a = mediaClip(0, 50);
        EffectInstance fx;
        fx.id = d.allocateId();
        fx.type = QStringLiteral("blur");
        a.effects.append(fx);
        ops::overwriteClip(d, kV1, a);
        ClipData b = mediaClip(60, 50);
        b.effects.append(d.videoTracks[0].clips[0].effects[0]);
        b.id = d.allocateId();
        d.videoTracks[0].clips.push_back(b); // the same effect id twice, as saved by 1.8.1
        QString why;
        QVERIFY(!ops::checkInvariants(d, &why));
        ops::normalizeIds(d);
        QVERIFY2(ops::checkInvariants(d, &why), qPrintable(why));
        QCOMPARE(d.videoTracks[0].clips[1].effects[0].type, QStringLiteral("blur"));
    }
};

QTEST_GUILESS_MAIN(TestEditOpsRipple)
#include "test_editops_ripple.moc"
