// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for multiple timelines per project, compound clips and nested timelines:
// collapse / decompose, recursion checks, undo, tabs, media pool items, save/load + migration.
#include "CoreTestUtils.h"
#include "core/CompoundOps.h"
#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Serialization.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

/// V1: [0,10) [10,20) linked to A1 [0,10) [10,20); V2: [5,15).
TimelineData sample()
{
    TimelineData d = timeline();
    d.uid = 1;
    const QVector<quint64> a = ops::overwriteClips(d,
        {{kV1, mediaClip(0, 10, 0, 100, QStringLiteral("a"))},
            {kA1, mediaClip(0, 10, 0, 100, QStringLiteral("a"))}});
    ops::linkClips(d, a);
    const QVector<quint64> b = ops::overwriteClips(d,
        {{kV1, mediaClip(10, 10, 20, 100, QStringLiteral("b"))},
            {kA1, mediaClip(10, 10, 20, 100, QStringLiteral("b"))}});
    ops::linkClips(d, b);
    ops::overwriteClip(d, kV2, mediaClip(5, 10, 0, 0, QStringLiteral("top")));
    return d;
}

QVector<quint64> allClipIds(const TimelineData& d)
{
    QVector<quint64> ids;
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        for (const TrackData& t : d.tracks(type)) {
            for (const ClipData& c : t.clips)
                ids.append(c.id);
        }
    }
    return ids;
}

int itemCount(const Project& p, MediaItemKind kind)
{
    return int(std::count_if(p.mediaPool()->items().begin(), p.mediaPool()->items().end(),
        [kind](const MediaItem& m) { return m.kind == kind; }));
}

} // namespace

class TestCompound : public QObject
{
    Q_OBJECT
private slots:
    // ---- Pure operations ----

    void collapseKeepsTimingTracksAndContent()
    {
        TimelineData d = sample();
        // Give clip "b" an effect, a keyframe and a transition to check they travel along.
        ClipData* b = ops::clipById(d, d.videoTracks[0].clips[1].id);
        b->transform.values[keys::Opacity].setKey(3, 50.0);
        b->transitionIn = TransitionData {};
        b->effects.append(
            EffectInstance {d.allocateId(), QStringLiteral("gaussian_blur"), true, {}});
        const TimelineData before = d;
        const auto result = compound::collapse(d, allClipIds(d), 7, QStringLiteral("CC"), nullptr);
        QVERIFY(result);
        KK_VERIFY_INVARIANTS(d);
        KK_VERIFY_INVARIANTS(result->nested);

        // Parent: one video instance on V1 and one linked audio instance on A1, [0, 20).
        QCOMPARE(layout(d, kV1), QStringLiteral("0:20"));
        QCOMPARE(layout(d, kA1), QStringLiteral("0:20"));
        QVERIFY(d.videoTracks[1].clips.empty());
        const ClipData& v = d.videoTracks[0].clips[0];
        const ClipData& a = d.audioTracks[0].clips[0];
        QCOMPARE(v.kind, ClipKind::Nested);
        QCOMPARE(v.nestedUid, quint64(7));
        QVERIFY(v.linkGroup != 0);
        QCOMPARE(a.linkGroup, v.linkGroup);
        QCOMPARE(result->instanceIds.size(), 2);

        // Nested: same relative timing and track order; clips identical to the originals.
        const TimelineData& n = result->nested;
        QVERIFY(n.compound);
        QCOMPARE(n.uid, quint64(7));
        QCOMPARE(n.videoTracks.size(), size_t(2));
        QCOMPARE(n.audioTracks.size(), size_t(1));
        QCOMPARE(layout(n, kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(n, kV2), QStringLiteral("5:10"));
        QCOMPARE(layout(n, kA1), QStringLiteral("0:10 10:10"));
        QCOMPARE(n.videoTracks[0].clips[1], before.videoTracks[0].clips[1]);
        QCOMPARE(n.audioTracks[0].clips[0].linkGroup, before.audioTracks[0].clips[0].linkGroup);
        QCOMPARE(n.duration(), int64_t(20));
        QCOMPARE(v.sourceDurationUs, 20 * kUsPerFrame25);
    }

    void collapseOffsetAndPartialSelection()
    {
        TimelineData d = timeline();
        d.uid = 1;
        const quint64 x = ops::overwriteClip(d, kV1, mediaClip(50, 10));
        const quint64 y = ops::overwriteClip(d, kV2, mediaClip(55, 20));
        ops::overwriteClip(d, kV1, mediaClip(100, 10)); // not selected, outside the range
        const auto r = compound::collapse(d, {x, y}, 2, QStringLiteral("CC"), nullptr);
        QVERIFY(r);
        QCOMPARE(layout(d, kV1), QStringLiteral("50:25 100:10"));
        QCOMPARE(layout(r->nested, kV1), QStringLiteral("0:10"));
        QCOMPARE(layout(r->nested, kV2), QStringLiteral("5:20"));
        QCOMPARE(r->nested.audioTracks.size(), size_t(1)); // always one audio track
        QCOMPARE(r->instanceIds.size(), 1); // no audio selected: no audio instance
    }

    void collapseAvoidsUnselectedClipInRange()
    {
        TimelineData d = timeline();
        d.uid = 1;
        const quint64 x = ops::overwriteClip(d, kV1, mediaClip(0, 10));
        ops::overwriteClip(d, kV1, mediaClip(10, 5)); // unselected, inside [0, 20)
        const quint64 y = ops::overwriteClip(d, kV1, mediaClip(15, 5));
        const auto r = compound::collapse(d, {x, y}, 2, QStringLiteral("CC"), nullptr);
        QVERIFY(r);
        QCOMPARE(layout(d, kV1), QStringLiteral("10:5"));
        QCOMPARE(layout(d, kV2), QStringLiteral("0:20")); // went up instead of overwriting
    }

    void collapseRefusesEmptyAndLocked()
    {
        TimelineData d = sample();
        const TimelineData before = d;
        QString error;
        QVERIFY(!compound::collapse(d, {}, 2, QStringLiteral("CC"), &error));
        QVERIFY(!error.isEmpty());
        d.videoTracks[1].locked = true;
        QVERIFY(!compound::collapse(d, allClipIds(d), 2, QStringLiteral("CC"), &error));
        d.videoTracks[1].locked = false;
        QCOMPARE(d, before);
    }

    void decomposeRestoresClipsExactly()
    {
        TimelineData d = sample();
        const TimelineData before = d;
        auto r = compound::collapse(d, allClipIds(d), 7, QStringLiteral("CC"), nullptr);
        QVERIFY(r);
        const auto restored = compound::decompose(d, r->instanceIds, r->nested, nullptr);
        QVERIFY(restored);
        KK_VERIFY_INVARIANTS(d);
        QCOMPARE(restored->size(), 5);
        for (TrackRef ref : {kV1, kV2, kA1})
            QCOMPARE(layout(d, ref), layout(before, ref));
        // Content identical apart from ids / link groups.
        const ClipData& orig = before.videoTracks[0].clips[1];
        ClipData back = d.videoTracks[0].clips[1];
        back.id = orig.id;
        back.linkGroup = orig.linkGroup;
        QCOMPARE(back, orig);
        // The pairs stay linked to each other (V1/A1 clip "a").
        QCOMPARE(d.videoTracks[0].clips[0].linkGroup, d.audioTracks[0].clips[0].linkGroup);
        QVERIFY(d.videoTracks[0].clips[0].linkGroup != d.videoTracks[0].clips[1].linkGroup);
    }

    void decomposeTrimmedAndMovedInstance()
    {
        TimelineData d = sample();
        auto r = compound::collapse(d, allClipIds(d), 7, QStringLiteral("CC"), nullptr);
        QVERIFY(r);
        const quint64 v = r->instanceIds[0];
        // Trim 4 frames off the head (shows nested 4..20) and move it to 100.
        QCOMPARE(ops::trimHead(d, v, 4, false), int64_t(4));
        const ClipData* inst = ops::clipById(d, v);
        QCOMPARE(inst->sourceInUs, 4 * kUsPerFrame25);
        QVERIFY(ops::moveClips(d, {v}, 96, 0));
        const auto restored = compound::decompose(d, {v}, r->nested, nullptr);
        QVERIFY(restored);
        KK_VERIFY_INVARIANTS(d);
        // Nested V1 [0,10) [10,20) shown from 4 at 100: [100,106) [106,116); V2 [5,15) ->
        // [101,111).
        QCOMPARE(layout(d, kV1), QStringLiteral("100:6 106:10"));
        QCOMPARE(layout(d, kV2), QStringLiteral("101:10"));
        // The head-trimmed piece starts 4 frames later in its source.
        QCOMPARE(d.videoTracks[0].clips[0].sourceInUs, 4 * kUsPerFrame25);
    }

    void decomposeRefusesSpeedAndForeignClip()
    {
        TimelineData d = sample();
        auto r = compound::collapse(d, allClipIds(d), 7, QStringLiteral("CC"), nullptr);
        QVERIFY(r);
        ops::clipById(d, r->instanceIds[0])->speed = 2.0;
        QString error;
        QVERIFY(!compound::decompose(d, {r->instanceIds[0]}, r->nested, &error));
        QVERIFY(!error.isEmpty());
        TimelineData other = r->nested;
        other.uid = 99;
        ops::clipById(d, r->instanceIds[0])->speed = 1.0;
        QVERIFY(!compound::decompose(d, {r->instanceIds[0]}, other, &error));
    }

    void recursionIsDetected()
    {
        // 1 contains 2, 2 contains 3.
        QHash<quint64, TimelineData> all;
        for (quint64 uid : {1, 2, 3}) {
            TimelineData t = timeline();
            t.uid = uid;
            all.insert(uid, t);
        }
        auto nest = [&all](quint64 outer, quint64 inner) {
            ops::overwriteClip(all[outer], kV1, compound::makeNestedClip(all[inner], {}));
        };
        nest(1, 2);
        nest(2, 3);
        const compound::TimelineLookup lookup = [&all](quint64 uid) -> const TimelineData* {
            const auto it = all.constFind(uid);
            return it == all.constEnd() ? nullptr : &*it;
        };
        QVERIFY(compound::reaches(1, 3, lookup));
        QVERIFY(!compound::reaches(3, 1, lookup));
        QVERIFY(!compound::canNest(1, 1, lookup)); // itself
        QVERIFY(!compound::canNest(1, 3, lookup)); // 3 inside 1 inside 3
        QVERIFY(!compound::canNest(2, 3, lookup));
        QVERIFY(compound::canNest(3, 1, lookup)); // already inside, more instances are fine
        QVERIFY(!compound::canNest(42, 1, lookup)); // unknown
        QCOMPARE(compound::reachableTimelines(1, lookup), (QVector<quint64> {2, 3}));
    }

    // ---- Project ----

    void projectStartsWithOneTimelineItem()
    {
        Project p;
        QCOMPARE(p.timelineUids().size(), 1);
        QCOMPARE(p.activeTimelineUid(), p.timelineUids().first());
        QCOMPARE(itemCount(p, MediaItemKind::Timeline), 1);
        QCOMPARE(p.tabs(), (QVector<quint64> {p.activeTimelineUid()}));
        QVERIFY(!p.isDirty());
    }

    void createSwitchRenameDeleteTimelines()
    {
        Project p;
        const quint64 first = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        p.timeline()->setPlayhead(7);

        const quint64 second = p.createTimeline(QStringLiteral("B"));
        QVERIFY(second != first);
        QCOMPARE(itemCount(p, MediaItemKind::Timeline), 2);
        QCOMPARE(p.activeTimelineUid(), first); // creating does not activate

        QSignalSpy switched(&p, &Project::activeTimelineChanged);
        QVERIFY(p.setActiveTimeline(second));
        QCOMPARE(switched.count(), 1);
        QCOMPARE(p.timeline()->data().name, QStringLiteral("B"));
        QVERIFY(p.timeline()->data().videoTracks[0].clips.empty());
        QCOMPARE(p.timeline()->playhead(), int64_t(0));
        QCOMPARE(p.tabs(), (QVector<quint64> {first, second}));

        QVERIFY(p.setActiveTimeline(first));
        QCOMPARE(p.timeline()->data().videoTracks[0].clips.size(), size_t(1));
        QCOMPARE(p.timeline()->playhead(), int64_t(7)); // own playhead

        QVERIFY(p.renameTimeline(second, QStringLiteral("Second")));
        QCOMPARE(p.timelineData(second)->name, QStringLiteral("Second"));
        QCOMPARE(p.mediaPool()->item(p.timelineItemId(second))->name, QStringLiteral("Second"));
        p.undo();
        QCOMPARE(p.timelineData(second)->name, QStringLiteral("B"));
        p.redo();

        const quint64 copy = p.duplicateTimeline(first);
        QCOMPARE(p.timelineData(copy)->videoTracks[0].clips.size(), size_t(1));
        QCOMPARE(p.timelineData(copy)->name, QStringLiteral("Timeline 1 copy"));

        // Delete the active one: another becomes active; undo brings it back.
        QVERIFY(p.deleteTimeline(first));
        QVERIFY(!p.hasTimeline(first));
        QVERIFY(p.activeTimelineUid() != first);
        QVERIFY(p.mediaPool()->item(p.timelineItemId(first)) == nullptr);
        p.undo();
        QVERIFY(p.hasTimeline(first));
        QVERIFY(!p.timelineItemId(first).isEmpty());

        // The last regular timeline can never be deleted.
        QVERIFY(p.deleteTimeline(copy));
        QVERIFY(p.deleteTimeline(second));
        QCOMPARE(p.timelineUids(), (QVector<quint64> {first}));
        QVERIFY(!p.deleteTimeline(first));
    }

    void tabsOpenCloseMove()
    {
        Project p;
        const quint64 a = p.activeTimelineUid();
        const quint64 b = p.createTimeline({});
        const quint64 c = p.createTimeline({});
        QVERIFY(p.openTab(b));
        QVERIFY(p.openTab(c));
        QCOMPARE(p.tabs(), (QVector<quint64> {a, b, c}));
        p.moveTab(2, 0);
        QCOMPARE(p.tabs(), (QVector<quint64> {c, a, b}));
        QVERIFY(p.closeTab(c)); // active: the neighbour becomes active
        QCOMPARE(p.activeTimelineUid(), a);
        QVERIFY(p.closeTab(b));
        QVERIFY(!p.closeTab(a)); // last tab stays
        QCOMPARE(p.tabs(), (QVector<quint64> {a}));
        // View state per timeline.
        p.setViewState(b, TimelineViewState {3.5, 10, 20});
        QCOMPARE(p.viewState(b).pixelsPerFrame, 3.5);
    }

    void editsInOtherTimelineUndoWithSwitch()
    {
        Project p;
        const quint64 a = p.activeTimelineUid();
        const quint64 b = p.createTimeline({});
        QVERIFY(p.setActiveTimeline(b));
        p.beginEdit(QStringLiteral("Add"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        QVERIFY(p.setActiveTimeline(a));
        p.undo(); // made in b: switches there so the user sees it
        QCOMPARE(p.activeTimelineUid(), b);
        QVERIFY(p.timeline()->data().videoTracks[0].clips.empty());
        p.redo();
        QCOMPARE(p.timeline()->data().videoTracks[0].clips.size(), size_t(1));
    }

    void compoundClipInProject()
    {
        Project p;
        p.beginEdit(QStringLiteral("Setup"));
        TimelineData s = sample();
        s.uid = p.activeTimelineUid();
        s.name = p.timeline()->data().name;
        p.timeline()->mutableData() = s;
        p.endEdit();
        const int undoCount = p.undoStack()->count();

        QString error;
        const quint64 cc = p.createCompoundClip(
            allClipIds(p.timeline()->data()), QStringLiteral("My Compound"), {}, &error);
        QVERIFY2(cc != 0, qPrintable(error));
        QCOMPARE(p.undoStack()->count(), undoCount + 1); // one step
        QCOMPARE(itemCount(p, MediaItemKind::CompoundClip), 1);
        const QString itemId = p.timelineItemId(cc);
        const MediaItem* item = p.mediaPool()->item(itemId);
        QCOMPARE(item->name, QStringLiteral("My Compound"));
        QCOMPARE(item->info.durationUs, 20 * kUsPerFrame25);
        const ClipData inst = p.timeline()->data().videoTracks[0].clips[0];
        QCOMPARE(inst.mediaId, itemId);
        QCOMPARE(inst.nestedUid, cc);
        QVERIFY(!p.canNest(p.activeTimelineUid(), cc)); // parent inside its own child
        QVERIFY(p.canNest(cc, p.activeTimelineUid()));

        // The render snapshot carries the nested timeline.
        const auto snap = p.timeline()->snapshot();
        QVERIFY(snap->nested.find(cc));
        QCOMPARE(snap->nested.find(cc)->duration(), int64_t(20));
        QVERIFY(!p.timeline()->plainSnapshot()->nested.map);

        // Edit inside the compound timeline (its own tab): the instance learns the new length
        // and the parent's render snapshot shows the change.
        const quint64 parent = p.activeTimelineUid();
        QVERIFY(p.openTab(cc));
        p.beginEdit(QStringLiteral("Extend"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(20, 10));
        p.endEdit();
        QVERIFY(p.setActiveTimeline(parent));
        QCOMPARE(p.timeline()->data().videoTracks[0].clips[0].sourceDurationUs, 30 * kUsPerFrame25);
        QCOMPARE(p.timeline()->snapshot()->nested.find(cc)->duration(), int64_t(30));
        QCOMPARE(p.mediaPool()->item(itemId)->info.durationUs, 30 * kUsPerFrame25);

        // Decompose in place: one step, original layout back.
        const auto restored = p.decomposeCompoundClip(inst.id, &error);
        QVERIFY2(restored, qPrintable(error));
        QCOMPARE(layout(p.timeline()->data(), kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(p.timeline()->data(), kA1), QStringLiteral("0:10 10:10"));
        p.undo();
        QCOMPARE(layout(p.timeline()->data(), kV1), QStringLiteral("0:20"));

        // Undo the extension (made in the compound tab) and the creation.
        p.undo();
        QCOMPARE(p.activeTimelineUid(), cc);
        p.undo();
        QVERIFY(!p.hasTimeline(cc));
        QCOMPARE(p.activeTimelineUid(), parent);
        QCOMPARE(layout(p.timeline()->data(), kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(itemCount(p, MediaItemKind::CompoundClip), 0);
    }

    void renameInMediaPoolRenamesTimeline()
    {
        Project p;
        const quint64 uid = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Rename"));
        p.mediaPool()->renameItem(p.timelineItemId(uid), QStringLiteral("Edit A"));
        p.endEdit();
        QCOMPARE(p.timeline()->data().name, QStringLiteral("Edit A"));
    }

    void saveLoadRoundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.kkproj"));
        Project p;
        p.beginEdit(QStringLiteral("Setup"));
        TimelineData s = sample();
        s.uid = p.activeTimelineUid();
        p.timeline()->mutableData() = s;
        p.endEdit();
        const quint64 cc = p.createCompoundClip(
            {p.timeline()->data().videoTracks[1].clips[0].id}, QStringLiteral("Top"));
        QVERIFY(cc);
        const quint64 other = p.createTimeline(QStringLiteral("Other"));
        QVERIFY(p.setTimelineFormat(other, false, 1280, 720, Rational {30, 1}));
        QVERIFY(p.openTab(other));
        p.timeline()->setPlayhead(12);
        p.setViewState(other, TimelineViewState {2.0, 30, 0});
        QVERIFY(p.save(path));

        Project q;
        QString error;
        QVERIFY2(q.load(path, &error), qPrintable(error));
        QCOMPARE(q.timelineUids(), p.timelineUids());
        for (const quint64 uid : p.timelineUids())
            QCOMPARE(*q.timelineData(uid), *p.timelineData(uid));
        QCOMPARE(q.mediaPool()->state(), p.mediaPool()->state());
        QCOMPARE(q.activeTimelineUid(), other);
        QCOMPARE(q.tabs(), p.tabs());
        QCOMPARE(q.timeline()->playhead(), int64_t(12));
        QCOMPARE(q.viewState(other).pixelsPerFrame, 2.0);
        QCOMPARE(q.timelineData(other)->width, 1280);
        QVERIFY(!q.timelineData(other)->useProjectSettings);
        QVERIFY(!q.isDirty());
    }

    void migratesVersion2Project()
    {
        // A version-2 file: one "timeline" object, no timeline items in the media pool.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("old.kkproj"));
        TimelineData t = sample();
        t.uid = 0;
        t.name = QStringLiteral("Main Edit");
        QJsonObject root {{QStringLiteral("formatVersion"), 2},
            {QStringLiteral("project"),
                QJsonObject {{QStringLiteral("name"), QStringLiteral("Old")}}},
            {QStringLiteral("mediaPool"), QJsonObject {}},
            {QStringLiteral("timeline"), serialization::timelineToJson(t)}};
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(root).toJson());
        f.close();

        Project p;
        QString error;
        QVERIFY2(p.load(path, &error), qPrintable(error));
        QCOMPARE(p.timelineUids().size(), 1);
        TimelineData loaded = p.timeline()->data();
        QCOMPARE(loaded.uid, quint64(1));
        loaded.uid = 0;
        QCOMPARE(loaded, t); // exactly the old timeline
        QCOMPARE(itemCount(p, MediaItemKind::Timeline), 1);
        QCOMPARE(p.mediaPool()->items().first().name, QStringLiteral("Main Edit"));
        QCOMPARE(p.settings().width, t.width);
        QCOMPARE(p.tabs().size(), 1);
        QVERIFY(!p.isDirty());
    }

    void projectSettingsFollowedOnlyByFollowingTimelines()
    {
        Project p;
        const quint64 a = p.activeTimelineUid();
        const quint64 b = p.createTimeline({});
        QVERIFY(p.setTimelineFormat(b, false, 640, 480, Rational {25, 1}));
        ProjectSettings s = p.settings();
        s.width = 3840;
        s.height = 2160;
        p.setSettings(s);
        QCOMPARE(p.timelineData(a)->width, 3840);
        QCOMPARE(p.timelineData(b)->width, 640);
        p.undo();
        QCOMPARE(p.timelineData(a)->width, 1920);
        QCOMPARE(p.settings().width, 1920);
    }
};

QTEST_GUILESS_MAIN(TestCompound)
#include "test_compound.moc"
