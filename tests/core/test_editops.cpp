// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for EditOps: insert/overwrite/split/lift/ripple/move/trim/link/snap + invariants.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/ParamKeys.h"

#include <QRandomGenerator>
#include <QTest>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

class TestEditOps : public QObject
{
    Q_OBJECT
private slots:
    // ---- overwrite ----
    void overwriteIntoEmptyTrack()
    {
        TimelineData d = timeline();
        const quint64 id = ops::overwriteClip(d, kV1, mediaClip(10, 20));
        QVERIFY(id != 0);
        QCOMPARE(layout(d, kV1), QStringLiteral("10:20"));
        QVERIFY(ops::clipById(d, id));
        KK_VERIFY_INVARIANTS(d);
    }

    void overwriteSplitsStraddledClip()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 100, 0, 1000));
        ops::overwriteClip(d, kV1, mediaClip(40, 20));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:40 40:20 60:40"));
        // Right remainder shows the source from frame 60 on and has a new id.
        const ClipData& right = d.videoTracks[0].clips[2];
        QVERIFY(right.id != a);
        QCOMPARE(right.sourceInUs, 60 * kUsPerFrame25);
        KK_VERIFY_INVARIANTS(d);
    }

    void overwriteTrimsHeadAndTail()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 30));
        ops::overwriteClip(d, kV1, mediaClip(50, 30, 10));
        ops::overwriteClip(d, kV1, mediaClip(20, 40));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:20 20:40 60:20"));
        QCOMPARE(d.videoTracks[0].clips[2].sourceInUs, 20 * kUsPerFrame25);
        KK_VERIFY_INVARIANTS(d);
    }

    void overwriteReplacesCoveredClips()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(10, 5));
        ops::overwriteClip(d, kV1, mediaClip(20, 5));
        ops::overwriteClip(d, kV1, mediaClip(0, 50));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:50"));
        KK_VERIFY_INVARIANTS(d);
    }

    void overwriteRejectsInvalid()
    {
        TimelineData d = timeline();
        QCOMPARE(ops::overwriteClip(d, kV1, mediaClip(0, 0)), quint64(0));
        QCOMPARE(
            ops::overwriteClip(d, TrackRef {TrackType::Video, 7}, mediaClip(0, 5)), quint64(0));
        d.videoTracks[0].locked = true;
        QCOMPARE(ops::overwriteClip(d, kV1, mediaClip(0, 5)), quint64(0));
        QCOMPARE(layout(d, kV1), QString());
    }

    void overwriteKeepsFreeIdAndReplacesTakenId()
    {
        TimelineData d = timeline();
        ClipData c = mediaClip(0, 5);
        c.id = d.allocateId();
        const quint64 first = ops::overwriteClip(d, kV1, c);
        QCOMPARE(first, c.id);
        c.start = 10;
        const quint64 second = ops::overwriteClip(d, kV1, c);
        QVERIFY(second != first);
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- insert ----
    void insertSplitsAndRipplesAllTracks()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 100));
        ops::overwriteClip(d, kA1, mediaClip(0, 100));
        ops::overwriteClip(d, kV2, mediaClip(80, 10));
        ops::insertClip(d, kV1, mediaClip(50, 10));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:50 50:10 60:50"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:50 60:50"));
        QCOMPARE(layout(d, kV2), QStringLiteral("90:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    void insertTargetTrackOnly()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 100));
        ops::overwriteClip(d, kA1, mediaClip(0, 100));
        ops::insertClip(d, kV1, mediaClip(50, 10), ops::RippleScope::TargetTracks);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:50 50:10 60:50"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:100"));
        KK_VERIFY_INVARIANTS(d);
    }

    void insertSkipsLockedTracks()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kA1, mediaClip(0, 100));
        d.audioTracks[0].locked = true;
        ops::insertClip(d, kV1, mediaClip(50, 10));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:100"));
        KK_VERIFY_INVARIANTS(d);
    }

    void insertLinkedVideoAndAudio()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 40));
        ops::overwriteClip(d, kA1, mediaClip(0, 40));
        const QVector<quint64> ids = ops::insertClips(d,
            {ops::PlacedClip {kV1, mediaClip(20, 10)}, ops::PlacedClip {kA1, mediaClip(20, 10)}});
        QCOMPARE(ids.size(), 2);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:20 20:10 30:20"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:20 20:10 30:20"));
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- split ----
    void splitKeepsContentContinuous()
    {
        TimelineData d = timeline();
        ClipData c = mediaClip(10, 40, 5, 1000);
        c.transform.animated(keys::Opacity, 100).setKey(0, 0.0);
        c.transform.animated(keys::Opacity, 100).setKey(40, 100.0);
        c.transitionIn = TransitionData {};
        c.transitionOut = TransitionData {};
        const quint64 id = ops::overwriteClip(d, kV1, c);
        const ClipData before = *ops::clipById(d, id);

        const QVector<quint64> rights = ops::splitAt(d, 30);
        QCOMPARE(rights.size(), 1);
        const ClipData& left = *ops::clipById(d, id);
        const ClipData& right = *ops::clipById(d, rights[0]);
        QCOMPARE(left.end(), 30);
        QCOMPARE(right.start, 30);
        QCOMPARE(right.end(), 50);
        QVERIFY(left.transitionIn && !left.transitionOut);
        QVERIFY(!right.transitionIn && right.transitionOut);
        // Every timeline frame shows the same source time and parameter value as before.
        for (int64_t f = 10; f < 50; ++f) {
            const ClipData& now = f < 30 ? left : right;
            QCOMPARE(now.sourceTimeUsAt(f, d.fps), before.sourceTimeUsAt(f, d.fps));
            QCOMPARE(now.transform.value(keys::Opacity, now.localFrame(f)),
                before.transform.value(keys::Opacity, before.localFrame(f)));
        }
        KK_VERIFY_INVARIANTS(d);
    }

    void splitAtEdgesDoesNothing()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        QVERIFY(ops::splitAt(d, 10).isEmpty());
        QVERIFY(ops::splitAt(d, 20).isEmpty());
        QVERIFY(ops::splitAt(d, 5).isEmpty());
        QCOMPARE(layout(d, kV1), QStringLiteral("10:10"));
    }

    void splitRelinksRightHalves()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 50));
        const quint64 a = ops::overwriteClip(d, kA1, mediaClip(0, 50));
        const quint64 group = ops::linkClips(d, {v, a});
        QVERIFY(group != 0);
        const QVector<quint64> rights = ops::splitAt(d, 20);
        QCOMPARE(rights.size(), 2);
        const quint64 rightGroup = ops::clipById(d, rights[0])->linkGroup;
        QVERIFY(rightGroup != 0 && rightGroup != group);
        QCOMPARE(ops::clipById(d, rights[1])->linkGroup, rightGroup);
        QCOMPARE(ops::linkedClips(d, v).size(), 2);
        KK_VERIFY_INVARIANTS(d);
    }

    void splitClipsOnlySelected()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 50));
        ops::overwriteClip(d, kA1, mediaClip(0, 50));
        QCOMPARE(ops::splitClips(d, {v}, 25).size(), 1);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:25 25:25"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:50"));
    }

    void splitAtWithFilter()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 50));
        ops::overwriteClip(d, kA1, mediaClip(0, 50));
        ops::splitAt(
            d, 10, [](TrackRef ref, const TrackData&) { return ref.type == TrackType::Audio; });
        QCOMPARE(layout(d, kV1), QStringLiteral("0:50"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:10 10:40"));
    }

    // ---- lift / ripple delete ----
    void liftLeavesGap()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const quint64 b = ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kV1, mediaClip(20, 10));
        QCOMPARE(ops::liftClips(d, {b}), 1);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 20:10"));
    }

    void rippleDeleteClosesGaps()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        const quint64 c = ops::overwriteClip(d, kV1, mediaClip(25, 10));
        ops::overwriteClip(d, kV1, mediaClip(40, 10));
        ops::overwriteClip(d, kA1, mediaClip(40, 10));
        QCOMPARE(ops::rippleDeleteClips(d, {a, c}), 2);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 20:10"));
        QCOMPARE(layout(d, kA1), QStringLiteral("40:10")); // other tracks untouched
        KK_VERIFY_INVARIANTS(d);
    }

    void rippleDeleteIgnoresLockedTrack()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        d.videoTracks[0].locked = true;
        QCOMPARE(ops::rippleDeleteClips(d, {a}), 0);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10"));
    }

    // ---- move ----
    void moveHorizontally()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(10, 10));
        QVERIFY(ops::moveClips(d, {a}, 15));
        QCOMPARE(layout(d, kV1), QStringLiteral("25:10"));
        QVERIFY(ops::moveClips(d, {a}, -100)); // clamped at 0
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10"));
        QVERIFY(!ops::moveClips(d, {a}, -5)); // already at 0: nothing moves
        KK_VERIFY_INVARIANTS(d);
    }

    void moveOverwritesTarget()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(20, 30));
        QVERIFY(ops::moveClips(d, {a}, 30));
        QCOMPARE(layout(d, kV1), QStringLiteral("20:10 30:10 40:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    void moveGroupKeepsRelativePositions()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(5, 10));
        const quint64 b = ops::overwriteClip(d, kV1, mediaClip(15, 10));
        QVERIFY(ops::moveClips(d, {b, a}, -20)); // clamped to -5 for both
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:10"));
        // Moving adjacent clips right must not let them overwrite each other.
        QVERIFY(ops::moveClips(d, {a, b}, 3));
        QCOMPARE(layout(d, kV1), QStringLiteral("3:10 13:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    void moveBetweenTracks()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const quint64 a = ops::overwriteClip(d, kA1, mediaClip(0, 10));
        QVERIFY(ops::moveClips(d, {v, a}, 0, 1));
        QCOMPARE(layout(d, kV1), QString());
        QCOMPARE(layout(d, kV2), QStringLiteral("0:10"));
        QCOMPARE(layout(d, kA2), QStringLiteral("0:10"));
        // Already on the top track: vertical delta clamps to 0; horizontal still applies.
        QVERIFY(ops::moveClips(d, {v}, 5, 3));
        QCOMPARE(layout(d, kV2), QStringLiteral("5:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    void moveRefusesLockedTracks()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        d.videoTracks[1].locked = true;
        QVERIFY(!ops::moveClips(d, {v}, 0, 1));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10"));
        d.videoTracks[0].locked = true;
        QVERIFY(!ops::moveClips(d, {v}, 5));
    }

    // ---- trim ----
    void trimHeadNonRipple()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const quint64 b = ops::overwriteClip(d, kV1, mediaClip(20, 10, 15, 100));
        QCOMPARE(ops::trimHead(d, b, 3, false), 3); // shorten
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 23:7"));
        QCOMPARE(ops::clipById(d, b)->sourceInUs, 18 * kUsPerFrame25);
        QCOMPARE(ops::trimHead(d, b, -100, false), -13); // stops at the left neighbour (frame 10)
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10 10:20"));
        QCOMPARE(ops::clipById(d, b)->sourceInUs, 5 * kUsPerFrame25);
        QCOMPARE(ops::trimHead(d, b, 100, false), 19); // keeps 1 frame
        QCOMPARE(ops::clipById(d, b)->duration, 1);
        KK_VERIFY_INVARIANTS(d);
    }

    void trimHeadLimitedBySource()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(50, 10, 4, 100));
        QCOMPARE(ops::trimHead(d, a, -10, false), -4);
        QCOMPARE(ops::clipById(d, a)->sourceInUs, 0);
        QCOMPARE(layout(d, kV1), QStringLiteral("46:14"));
    }

    void trimHeadMovesKeyframesWithContent()
    {
        TimelineData d = timeline();
        ClipData c = mediaClip(0, 50, 0, 1000);
        c.transform.animated(keys::PosX, 0).setKey(20, 5.0);
        const quint64 a = ops::overwriteClip(d, kV1, c);
        ops::trimHead(d, a, 10, false);
        QVERIFY(ops::clipById(d, a)->transform.values[keys::PosX].hasKeyAt(10));
    }

    void trimHeadRipple()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 20, 10, 100));
        ops::overwriteClip(d, kV1, mediaClip(30, 10));
        QCOMPARE(ops::trimHead(d, a, 5, true), 5);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:15 25:10"));
        QCOMPARE(ops::clipById(d, a)->sourceInUs, 15 * kUsPerFrame25);
        QCOMPARE(ops::trimHead(d, a, -100, true), -15); // source-limited, later clip shifts
        QCOMPARE(layout(d, kV1), QStringLiteral("0:30 40:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    void trimTailNonRipple()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10, 0, 100));
        ops::overwriteClip(d, kV1, mediaClip(15, 10));
        QCOMPARE(ops::trimTail(d, a, 20, false), 5); // neighbour at 15
        QCOMPARE(layout(d, kV1), QStringLiteral("0:15 15:10"));
        QCOMPARE(ops::trimTail(d, a, -50, false), -14);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:1 15:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    void trimTailLimitedBySource()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10, 85, 100));
        QCOMPARE(ops::trimTail(d, a, 50, false), 5);
        QCOMPARE(ops::maxTailExtension(*ops::clipById(d, a), d.fps), 0);
    }

    void trimTailRipple()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        QCOMPARE(ops::trimTail(d, a, 7, true), 7); // unbounded source
        QCOMPARE(layout(d, kV1), QStringLiteral("0:17 17:10"));
        QCOMPARE(ops::trimTail(d, a, -12, true), -12);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:5 5:10"));
        KK_VERIFY_INVARIANTS(d);
    }

    void trimRespectsSpeed()
    {
        TimelineData d = timeline();
        ClipData c = mediaClip(0, 10, 0, 100);
        c.speed = 2.0; // 10 frames consume 20 source frames
        const quint64 a = ops::overwriteClip(d, kV1, c);
        QCOMPARE(ops::maxTailExtension(*ops::clipById(d, a), d.fps), 40);
    }

    void slip()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(0, 10, 5, 20));
        QCOMPARE(ops::slipClip(d, a, 100), 5);
        QCOMPARE(ops::clipById(d, a)->sourceInUs, 10 * kUsPerFrame25);
        QCOMPARE(ops::slipClip(d, a, -100), -10);
        QCOMPARE(ops::clipById(d, a)->sourceInUs, 0);
        QCOMPARE(layout(d, kV1), QStringLiteral("0:10"));
    }

    // ---- tracks ----
    void addAndRemoveTracks()
    {
        TimelineData d = timeline();
        QCOMPARE(ops::addTrack(d, TrackType::Video), 2);
        QCOMPARE(ops::addTrack(d, TrackType::Audio, 0), 0);
        QCOMPARE(d.videoTracks.size(), size_t(3));
        QCOMPARE(d.audioTracks.size(), size_t(3));
        QCOMPARE(trackDisplayName(d.videoTracks[2], 2), QStringLiteral("V3"));
        ops::overwriteClip(d, kV2, mediaClip(0, 10));
        QVERIFY(ops::removeTrack(d, kV2));
        QCOMPARE(d.videoTracks.size(), size_t(2));
        QVERIFY(!ops::removeTrack(d, TrackRef {TrackType::Video, 9}));
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- linking ----
    void linkAndUnlink()
    {
        TimelineData d = timeline();
        const quint64 v = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const quint64 a1 = ops::overwriteClip(d, kA1, mediaClip(0, 10));
        const quint64 a2 = ops::overwriteClip(d, kA2, mediaClip(0, 10));
        QCOMPARE(ops::linkClips(d, {v}), quint64(0));
        ops::linkClips(d, {v, a1, a2});
        QCOMPARE(ops::linkedClips(d, a1).size(), 3);
        QCOMPARE(ops::withLinkedClips(d, {v, a2}).size(), 3);
        ops::unlinkClips(d, {a2});
        QCOMPARE(ops::linkedClips(d, v).size(), 2);
        ops::unlinkClips(d, {a1}); // leaves v alone → group dissolved
        QCOMPARE(ops::clipById(d, v)->linkGroup, quint64(0));
        KK_VERIFY_INVARIANTS(d);
    }

    // ---- snapping & navigation ----
    void snapping()
    {
        TimelineData d = timeline();
        const quint64 a = ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kA1, mediaClip(30, 10));
        d.markers.append(Marker {55, QStringLiteral("m"), {}});
        const auto all = ops::snapCandidates(d, 47);
        QCOMPARE(all, (std::vector<int64_t> {0, 10, 20, 30, 40, 47, 55}));
        const auto excluding = ops::snapCandidates(d, 47, {a});
        QCOMPARE(excluding, (std::vector<int64_t> {0, 30, 40, 47, 55}));
        QCOMPARE(ops::snapToNearest(all, 22, 3), std::optional<int64_t>(20));
        QCOMPARE(ops::snapToNearest(all, 25, 3), std::nullopt);
        QCOMPARE(ops::snapToNearest(all, 100, 50), std::optional<int64_t>(55));
        QCOMPARE(ops::snapToNearest({}, 5, 10), std::nullopt);
    }

    void snappingToClipMarkers()
    {
        // Owner request on 0.4.0: clip markers snap at their timeline frame, both ends of a
        // ranged one; markers trimmed out of view and those of excluded clips do not.
        TimelineData d = timeline();
        ClipData clip = mediaClip(100, 50);
        clip.markers.append(Marker {-5, QStringLiteral("hidden"), {}}); // before the head
        clip.markers.append(Marker {10, QStringLiteral("single"), {}});
        Marker ranged {20, QStringLiteral("ranged"), {}};
        ranged.duration = 15;
        clip.markers.append(ranged);
        const quint64 a = ops::overwriteClip(d, kV1, clip);
        QCOMPARE(ops::snapCandidates(d, 0), (std::vector<int64_t> {0, 100, 110, 120, 135, 150}));
        QCOMPARE(ops::snapCandidates(d, 0, {a}), (std::vector<int64_t> {0}));
        QCOMPARE(ops::nextEditPoint(d, 100), std::optional<int64_t>(150)); // not edit points
    }

    void editPoints()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(10, 10));
        ops::overwriteClip(d, kA1, mediaClip(15, 20));
        QCOMPARE(ops::nextEditPoint(d, 0), std::optional<int64_t>(10));
        QCOMPARE(ops::nextEditPoint(d, 10), std::optional<int64_t>(15));
        QCOMPARE(ops::nextEditPoint(d, 35), std::nullopt);
        QCOMPARE(ops::previousEditPoint(d, 20), std::optional<int64_t>(15));
        QCOMPARE(ops::previousEditPoint(d, 10), std::nullopt);
        QCOMPARE(d.duration(), 35);
    }

    // ---- invariants ----
    void invariantCheckerDetectsProblems()
    {
        TimelineData d = timeline();
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(20, 10));
        KK_VERIFY_INVARIANTS(d);

        TimelineData overlap = d;
        overlap.videoTracks[0].clips[1].start = 5;
        QString why;
        QVERIFY(!ops::checkInvariants(overlap, &why));
        QVERIFY(why.contains(QLatin1String("overlaps")));

        TimelineData duplicate = d;
        duplicate.videoTracks[0].clips[1].id = duplicate.videoTracks[0].clips[0].id;
        QVERIFY(!ops::checkInvariants(duplicate));

        TimelineData empty = d;
        empty.videoTracks[0].clips[0].duration = 0;
        QVERIFY(!ops::checkInvariants(empty));

        TimelineData staleIds = d;
        staleIds.nextId = 1;
        QVERIFY(!ops::checkInvariants(staleIds));
        ops::normalizeIds(staleIds);
        KK_VERIFY_INVARIANTS(staleIds);
    }

    /// Random sequences of operations must never break the invariants.
    void randomOperationsKeepInvariants()
    {
        QRandomGenerator rng(1234);
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
                ops::overwriteClip(d, anyTrack(), mediaClip(f, len, rng.bounded(50), 200));
                break;
            case 1:
                ops::insertClip(d, anyTrack(), mediaClip(f, len),
                    rng.bounded(2) ? ops::RippleScope::AllTracks : ops::RippleScope::TargetTracks);
                break;
            case 2:
                ops::splitAt(d, f);
                break;
            case 3:
                ops::moveClips(d, {anyClip(), anyClip()}, rng.bounded(-30, 30), rng.bounded(-1, 2));
                break;
            case 4:
                ops::trimHead(d, anyClip(), rng.bounded(-20, 20), rng.bounded(2));
                break;
            case 5:
                ops::trimTail(d, anyClip(), rng.bounded(-20, 20), rng.bounded(2));
                break;
            case 6:
                ops::rippleDeleteClips(d, {anyClip()});
                break;
            case 7:
                ops::liftClips(d, {anyClip()});
                break;
            case 8:
                ops::linkClips(d, {anyClip(), anyClip()});
                break;
            case 9:
                ops::slipClip(d, anyClip(), rng.bounded(-10, 10));
                break;
            }
            QString why;
            if (!ops::checkInvariants(d, &why))
                QFAIL(qPrintable(QStringLiteral("step %1: %2").arg(step).arg(why)));
        }
    }
};

QTEST_GUILESS_MAIN(TestEditOps)
#include "test_editops.moc"
