// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for multicam clips (core/Multicam.h, Project): building the multicam timeline
// from synced sources, angle views, the viewer grid, timecode / in-point offsets, cut and
// switch angle edits with undo, flatten, render snapshots, save / load and migration.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/Multicam.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Serialization.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;
namespace mc = kk::multicam;

namespace {

constexpr Rational kFps {25, 1};

media::MediaInfo info(double seconds, bool video = true, bool audio = true)
{
    media::MediaInfo i;
    i.valid = true;
    i.hasVideo = video;
    i.hasAudio = audio;
    i.width = i.displayWidth = 1920;
    i.height = i.displayHeight = 1080;
    i.frameRate = kFps;
    i.durationUs = int64_t(seconds * 1e6);
    return i;
}

mc::AngleSource source(
    const QString& id, double seconds, int64_t offsetUs, bool video = true, bool audio = true)
{
    return {id, id, info(seconds, video, audio), offsetUs};
}

/// A project with a three-angle multicam clip (A: 20 s, B: 15 s starting 2 s later,
/// C: audio only, 1.5 s before A) and the multicam clip on the active timeline at frame 0.
struct Fixture
{
    Project project;
    quint64 mcUid = 0;
    quint64 video = 0; ///< the multicam clip on V1
    quint64 audio = 0; ///< its linked part on A1

    Fixture()
    {
        QString error;
        mcUid = project.createMulticamClip(
            {source(QStringLiteral("a"), 20, 0), source(QStringLiteral("b"), 15, 2000000),
                source(QStringLiteral("c"), 25, -1500000, false, true)},
            QStringLiteral("Concert"), kFps, 1920, 1080, {}, false, &error);
        QVERIFY2(mcUid, qPrintable(error));
        const auto data = project.timelineData(mcUid);
        project.beginEdit(QStringLiteral("Place"));
        TimelineData& d = project.timeline()->mutableData();
        ClipData clip = mc::makeMulticamClip(*data, project.timelineItemId(mcUid), 0);
        const QVector<quint64> ids = ops::overwriteClips(d, {{kV1, clip}, {kA1, clip}});
        ops::linkClips(d, ids);
        video = ids[0];
        audio = ids[1];
        project.endEdit();
    }
    const TimelineData& data() const { return project.timeline()->data(); }
    const ClipData& clip(quint64 id) const { return *ops::clipById(data(), id); }
};

} // namespace

class TestMulticam : public QObject
{
    Q_OBJECT

private slots:
    void buildPlacesSyncedSources()
    {
        QString error;
        const auto t = mc::build(
            {source(QStringLiteral("a"), 20, 0), source(QStringLiteral("b"), 15, 2010000),
                source(QStringLiteral("c"), 25, -1500000, false, true)},
            7, QStringLiteral("MC"), kFps, 1920, 1080, &error);
        QVERIFY2(t, qPrintable(error));
        QVERIFY(t->multicam);
        QVERIFY(t->compound);
        QCOMPARE(t->uid, quint64(7));
        QCOMPARE(mc::angleCount(*t), 3);
        QCOMPARE(t->videoTracks.size(), size_t(3));
        QCOMPARE(t->audioTracks.size(), size_t(3));
        QCOMPARE(mc::angleName(*t, 1), QStringLiteral("b"));
        // C starts first (offset -1.5 s): frame 0. A at 1.5 s = frame 37.5 -> 38, skipping
        // 20 ms of its head so its time 0 stays at 1.5 s exactly.
        const ClipData& a = t->videoTracks[0].clips[0];
        QCOMPARE(a.start, int64_t(38));
        QCOMPARE(a.sourceInUs, int64_t(20000));
        QCOMPARE(a.duration, int64_t(500)); // 19.98 s rounded up
        QCOMPARE(t->audioTracks[0].clips[0].linkGroup, a.linkGroup);
        QVERIFY(a.linkGroup != 0);
        const ClipData& b = t->videoTracks[1].clips[0];
        // B at 3.51 s = frame 87.75 -> 88, head 10 ms.
        QCOMPARE(b.start, int64_t(88));
        QCOMPARE(b.sourceInUs, int64_t(10000));
        QVERIFY(t->videoTracks[2].clips.empty()); // C has no video
        QCOMPARE(t->audioTracks[2].clips[0].start, int64_t(0));
        QCOMPARE(t->audioTracks[2].clips[0].linkGroup, quint64(0));
        KK_VERIFY_INVARIANTS(*t);

        QVERIFY(!mc::build({}, 1, {}, kFps, 1920, 1080, &error));
        media::MediaInfo still = info(0);
        still.isStillImage = true;
        QVERIFY(!mc::build({{QStringLiteral("s"), QStringLiteral("s"), still, 0}}, 1, {}, kFps,
            1920, 1080, &error));
    }

    void placeSyncedStacksTracks()
    {
        TimelineData d = timeline(); // V1, V2, A1, A2
        ops::overwriteClip(d, kV1, mediaClip(0, 10));
        const auto ids = mc::placeSynced(d,
            {source(QStringLiteral("a"), 10, 1000000), source(QStringLiteral("b"), 4, 0),
                source(QStringLiteral("c"), 6, 3000000, false, true)},
            50);
        QVERIFY(ids);
        QCOMPARE(ids->size(), 5); // a: V+A, b: V+A, c: A
        QCOMPARE(d.videoTracks.size(), size_t(4));
        QCOMPARE(d.audioTracks.size(), size_t(5));
        QCOMPARE(d.videoTracks[2].name, QStringLiteral("a"));
        QCOMPARE(d.videoTracks[2].clips[0].start, int64_t(75)); // 1 s after b
        QCOMPARE(d.videoTracks[3].clips[0].start, int64_t(50));
        QCOMPARE(d.audioTracks[4].name, QStringLiteral("c"));
        QCOMPARE(d.audioTracks[4].clips[0].start, int64_t(125));
        QCOMPARE(ops::linkedClips(d, d.videoTracks[2].clips[0].id).size(), 2);
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(1)); // existing content untouched
        KK_VERIFY_INVARIANTS(d);
    }

    void angleViewHoldsOneAngle()
    {
        const auto t
            = mc::build({source(QStringLiteral("a"), 10, 0), source(QStringLiteral("b"), 10, 0)}, 3,
                {}, kFps, 1920, 1080);
        const TimelineData v = mc::angleView(*t, 1);
        QCOMPARE(v.uid, quint64(3));
        QCOMPARE(v.videoTracks.size(), size_t(1));
        QCOMPARE(v.audioTracks.size(), size_t(1));
        QCOMPARE(v.videoTracks[0].clips[0].mediaId, QStringLiteral("b"));
        QCOMPARE(v.duration(), t->duration());
        QVERIFY(mc::angleView(*t, 5).videoTracks.empty());
    }

    void gridLayout()
    {
        const auto t
            = mc::build({source(QStringLiteral("a"), 10, 0), source(QStringLiteral("b"), 10, 0),
                            source(QStringLiteral("c"), 10, 0)},
                3, {}, kFps, 1920, 1080);
        QCOMPARE(mc::columnsFor(3), 2);
        QCOMPARE(mc::columnsFor(5), 3);
        QCOMPARE(mc::columnsFor(16), 4);
        const TimelineData g = mc::gridTimeline(*t, 2, 0, 1, QStringLiteral("item"));
        QCOMPARE(g.videoTracks.size(), size_t(3)); // the fourth cell is empty
        QCOMPARE(g.duration(), t->duration());
        const ClipData& topRight = g.videoTracks[1].clips[0];
        QCOMPARE(topRight.multicamAngle, 1);
        QCOMPARE(topRight.nestedUid, quint64(3));
        QCOMPARE(topRight.transform.value(keys::ZoomX, 0), 0.5);
        QCOMPARE(topRight.transform.value(keys::PosX, 0), 480.0);
        QCOMPARE(topRight.transform.value(keys::PosY, 0), -270.0);
        QCOMPARE(g.audioTracks[0].clips[0].multicamAngle, 1);
        QCOMPARE(mc::angleAtCell(3, 2, 0, 0, 1), 2);
        QCOMPARE(mc::angleAtCell(3, 2, 0, 1, 1), -1);
        QCOMPARE(mc::angleAtCell(10, 3, 1, 0, 0), 9);
        // Page 2 of a 2x2 grid of 3 angles has nothing but the audio track.
        QVERIFY(mc::gridTimeline(*t, 2, 1, 0, {}).videoTracks.empty());
    }

    void timecodeParsing()
    {
        QCOMPARE(mc::timecodeToUs(QStringLiteral("00:00:01:00"), kFps), int64_t(1000000));
        QCOMPARE(
            mc::timecodeToUs(QStringLiteral("01:00:00:12"), kFps), int64_t(3600000000LL + 480000));
        QVERIFY(!mc::timecodeToUs(QStringLiteral("00:00:00:25"), kFps)); // frame out of range
        QVERIFY(!mc::timecodeToUs(QStringLiteral("garbage"), kFps));
        QVERIFY(!mc::timecodeToUs(QString(), kFps));
        // 29.97 non drop: 30 frame numbers per timecode second, played at 29.97.
        const Rational ntsc {30000, 1001};
        QCOMPARE(mc::timecodeToUs(QStringLiteral("00:00:01:00"), ntsc), framesToUs(30, ntsc));
        // Drop frame: 00:01:00;02 is the 1800th frame (numbers 00 and 01 were skipped).
        QCOMPARE(mc::timecodeToUs(QStringLiteral("00:01:00;02"), ntsc), framesToUs(1800, ntsc));
        // Ten minutes drop frame = 17982 frames = 600 s of real time (to 1 frame).
        QCOMPARE(mc::timecodeToUs(QStringLiteral("00:10:00;00"), ntsc), framesToUs(17982, ntsc));
        QVERIFY(
            std::abs(*mc::timecodeToUs(QStringLiteral("00:10:00;00"), ntsc) - 600000000LL) < 40000);

        media::MediaInfo a = info(10), b = info(10), c = info(10, false, true);
        a.startTimecode = QStringLiteral("10:00:00:00");
        b.startTimecode = QStringLiteral("10:00:02:05");
        c.frameRate = {}; // audio only: counted at 25
        c.startTimecode = QStringLiteral("09:59:59:00");
        const auto offsets = mc::offsetsFromTimecode({a, b, c, info(5)});
        QCOMPARE(*offsets[1] - *offsets[0], int64_t(2200000));
        QCOMPARE(*offsets[2] - *offsets[0], int64_t(-1000000));
        QVERIFY(!offsets[3]);

        const QVector<int64_t> in = mc::offsetsFromInPoints({500000, 0, 2000000});
        QCOMPARE(in, (QVector<int64_t> {-500000, 0, -2000000}));
    }

    void projectCreatesItemAndBin()
    {
        Project p;
        const QString binId = p.mediaPool()->addBin(QStringLiteral("Shoot"));
        MediaItem a;
        a.name = QStringLiteral("CamA.mov");
        a.info = info(10);
        const QString ida = p.mediaPool()->addItem(a);
        const quint64 uid = p.createMulticamClip(
            {{ida, QStringLiteral("CamA"), a.info, 0}}, QString(), kFps, 1920, 1080, binId, true);
        QVERIFY(uid);
        const MediaItem* item = p.mediaPool()->item(p.timelineItemId(uid));
        QVERIFY(item);
        QCOMPARE(item->kind, MediaItemKind::MulticamClip);
        QCOMPARE(item->binId, binId);
        QCOMPARE(item->name, QStringLiteral("Multicam Clip 1"));
        // The source moved into "Original Clips" inside the target bin.
        const QString moved = p.mediaPool()->item(ida)->binId;
        QVERIFY(moved != binId && !moved.isEmpty());
        const auto bins = p.mediaPool()->bins();
        const auto bin
            = std::find_if(bins.cbegin(), bins.cend(), [&](const Bin& b) { return b.id == moved; });
        QCOMPARE(bin->name, QStringLiteral("Original Clips"));
        QCOMPARE(bin->parentId, binId);
        // One undo step removes the clip, its item and the bin move.
        p.undo();
        QVERIFY(!p.hasTimeline(uid));
        QCOMPARE(p.mediaPool()->item(ida)->binId, QString());
        p.redo();
        QVERIFY(p.hasTimeline(uid));
        // A multicam timeline is not a regular timeline (cannot be the last one standing).
        QVERIFY(p.deleteTimeline(uid));
    }

    void snapshotCarriesAngleViews()
    {
        Fixture f;
        const auto shot = f.project.timeline()->snapshot();
        const TimelineData* view = shot->nested.findClip(f.clip(f.video));
        QVERIFY(view);
        QCOMPARE(view->videoTracks.size(), size_t(1));
        QCOMPARE(view->videoTracks[0].name, QStringLiteral("a"));
        // Without an angle the whole timeline.
        ClipData plain = f.clip(f.video);
        plain.multicamAngle = -1;
        QCOMPARE(shot->nested.findClip(plain)->videoTracks.size(), size_t(3));
        // Angle 1 view, via a project render snapshot too (timeline thumbnails).
        const auto angle1 = f.project.renderSnapshot(multicamViewKey(f.mcUid, 1));
        QVERIFY(angle1);
        QCOMPARE(angle1->videoTracks[0].name, QStringLiteral("b"));
        // A private timeline (uid 0, the source viewer's grid) reaches the multicam clip too.
        const TimelineData grid
            = mc::gridTimeline(*f.project.timelineData(f.mcUid), 2, 0, 0, QString());
        const NestedTimelines n = f.project.nestedTimelinesFor(grid);
        QVERIFY(n.find(f.mcUid));
        QVERIFY(n.findClip(grid.videoTracks[2].clips[0]));
        QCOMPARE(
            n.findClip(grid.videoTracks[2].clips[0])->audioTracks[0].name, QStringLiteral("c"));
    }

    void switchAngle()
    {
        Fixture f;
        auto lk = f.project.timelineLookup();
        f.project.beginEdit(QStringLiteral("Switch"));
        QVERIFY(mc::switchAngle(
            f.project.timeline()->mutableData(), f.video, 1, mc::AngleTarget::Video, lk));
        f.project.endEdit();
        QCOMPARE(f.clip(f.video).multicamAngle, 1);
        QCOMPARE(f.clip(f.audio).multicamAngle, 0);
        f.project.beginEdit(QStringLiteral("Switch"));
        QVERIFY(mc::switchAngle(
            f.project.timeline()->mutableData(), f.audio, 2, mc::AngleTarget::Both, lk));
        f.project.endEdit();
        QCOMPARE(f.clip(f.video).multicamAngle, 2);
        QCOMPARE(f.clip(f.audio).multicamAngle, 2);
        // Nothing to do / bad angle / not a multicam clip.
        TimelineData& d = f.project.timeline()->mutableData();
        QVERIFY(!mc::switchAngle(d, f.video, 2, mc::AngleTarget::Both, lk));
        QVERIFY(!mc::switchAngle(d, f.video, 3, mc::AngleTarget::Both, lk));
        QVERIFY(!mc::switchAngle(d, 9999, 0, mc::AngleTarget::Both, lk));
        f.project.timeline()->notifyChanged();
        f.project.undo();
        QCOMPARE(f.clip(f.video).multicamAngle, 1);
        QCOMPARE(f.clip(f.audio).multicamAngle, 0);
        f.project.undo();
        QCOMPARE(f.clip(f.video).multicamAngle, 0);
    }

    void cutToAngle()
    {
        Fixture f;
        auto lk = f.project.timelineLookup();
        const int64_t length = f.clip(f.video).duration;
        // Cut both at frame 100 to angle 1.
        f.project.beginEdit(QStringLiteral("Cut"));
        QVector<quint64> shown = mc::cutToAngle(
            f.project.timeline()->mutableData(), 100, 1, mc::AngleTarget::Both, lk);
        f.project.endEdit();
        QCOMPARE(shown.size(), 2);
        QCOMPARE(layout(f.data(), kV1), QStringLiteral("0:100 100:%1").arg(length - 100));
        QCOMPARE(layout(f.data(), kA1), QStringLiteral("0:100 100:%1").arg(length - 100));
        QCOMPARE(f.data().videoTracks[0].clips[1].multicamAngle, 1);
        QCOMPARE(f.data().audioTracks[0].clips[1].multicamAngle, 1);
        QCOMPARE(f.data().videoTracks[0].clips[0].multicamAngle, 0);
        // The right parts are linked with each other, not with the left ones.
        const quint64 rightV = f.data().videoTracks[0].clips[1].id;
        QCOMPARE(ops::linkedClips(f.data(), rightV).size(), 2);
        QCOMPARE(ops::linkedClips(f.data(), f.video).size(), 2);
        KK_VERIFY_INVARIANTS(f.data());

        // Video-only cut at 200 to angle 2: both sides are cut, only the video switches.
        f.project.beginEdit(QStringLiteral("Cut"));
        shown = mc::cutToAngle(
            f.project.timeline()->mutableData(), 200, 2, mc::AngleTarget::Video, lk);
        f.project.endEdit();
        QCOMPARE(shown.size(), 1);
        QCOMPARE(f.data().videoTracks[0].clips[2].multicamAngle, 2);
        QCOMPARE(f.data().audioTracks[0].clips[2].multicamAngle, 1);

        // A cut on a segment's first frame switches it without a cut.
        f.project.beginEdit(QStringLiteral("Cut"));
        shown = mc::cutToAngle(
            f.project.timeline()->mutableData(), 200, 0, mc::AngleTarget::Audio, lk);
        f.project.endEdit();
        QCOMPARE(shown.size(), 1);
        QCOMPARE(f.data().audioTracks[0].clips.size(), size_t(3));
        QCOMPARE(f.data().audioTracks[0].clips[2].multicamAngle, 0);

        // Same angle again: nothing.
        QVERIFY(
            mc::cutToAngle(f.project.timeline()->mutableData(), 250, 2, mc::AngleTarget::Video, lk)
                .isEmpty());
        QCOMPARE(f.data().videoTracks[0].clips.size(), size_t(3));
        // Outside any multicam clip: nothing.
        QVERIFY(mc::cutToAngle(
            f.project.timeline()->mutableData(), length + 5, 1, mc::AngleTarget::Both, lk)
                    .isEmpty());
        f.project.timeline()->notifyChanged();
        KK_VERIFY_INVARIANTS(f.data());

        // Each cut is one undo step.
        f.project.undo();
        QCOMPARE(f.data().audioTracks[0].clips[2].multicamAngle, 1);
        f.project.undo();
        QCOMPARE(f.data().videoTracks[0].clips.size(), size_t(2));
        f.project.undo();
        QCOMPARE(f.data().videoTracks[0].clips.size(), size_t(1));
    }

    void coexistsWithSubtitleTracks()
    {
        // Captions stay on the root timeline: a cut leaves the subtitle track alone, and the
        // angle views never carry subtitle tracks (a caption track added inside the multicam
        // timeline is not shown through an angle).
        Fixture f;
        f.project.beginEdit(QStringLiteral("Captions"));
        TimelineData& d = f.project.timeline()->mutableData();
        TrackData st;
        st.id = d.allocateId();
        st.type = TrackType::Subtitle;
        ClipData caption;
        caption.id = d.allocateId();
        caption.kind = ClipKind::Subtitle;
        caption.start = 80;
        caption.duration = 50;
        caption.title.setStatic(keys::Text, QStringLiteral("Hello"));
        st.clips.push_back(caption);
        d.subtitleTracks.push_back(st);
        f.project.endEdit();
        f.project.beginEdit(QStringLiteral("Cut"));
        QVERIFY(!mc::cutToAngle(f.project.timeline()->mutableData(), 100, 1, mc::AngleTarget::Both,
            f.project.timelineLookup())
                     .isEmpty());
        f.project.endEdit();
        QCOMPARE(f.data().subtitleTracks.size(), size_t(1));
        QCOMPARE(f.data().subtitleTracks[0].clips.size(), size_t(1));
        QCOMPARE(f.data().subtitleTracks[0].clips[0].start, int64_t(80));
        QCOMPARE(f.data().videoTracks[0].clips.size(), size_t(2));
        KK_VERIFY_INVARIANTS(f.data());
        TimelineData m = *f.project.timelineData(f.mcUid);
        m.subtitleTracks.push_back(st);
        QVERIFY(mc::angleView(m, 0).subtitleTracks.empty());
        QVERIFY(mc::gridTimeline(m, 2, 0, 0, {}).subtitleTracks.empty());
    }

    void cutRespectsLockedTracks()
    {
        Fixture f;
        f.project.beginEdit(QStringLiteral("Lock"));
        f.project.timeline()->mutableData().audioTracks[0].locked = true;
        f.project.endEdit();
        TimelineData d = f.data();
        QVERIFY(
            mc::cutToAngle(d, 50, 1, mc::AngleTarget::Both, f.project.timelineLookup()).isEmpty());
        QCOMPARE(d, f.data());
    }

    void multicamClipAtPrefersTopVideo()
    {
        Fixture f;
        auto lk = f.project.timelineLookup();
        QCOMPARE(mc::multicamClipAt(f.data(), 10, lk), f.video);
        QCOMPARE(mc::multicamClipAt(f.data(), 10, lk, true), f.audio);
        QCOMPARE(mc::multicamClipAt(f.data(), 100000, lk), quint64(0));
        QVERIFY(mc::isMulticamClip(f.clip(f.video), lk));
    }

    void flatten()
    {
        Fixture f;
        auto lk = f.project.timelineLookup();
        f.project.beginEdit(QStringLiteral("Cut"));
        TimelineData& d = f.project.timeline()->mutableData();
        mc::cutToAngle(d, 100, 1, mc::AngleTarget::Both, lk);
        f.project.endEdit();
        // Flatten the second segment (angle 1 = B, which starts at frame 88 of the multicam
        // timeline): it becomes B's clip at the same frames, V and A linked.
        const quint64 seg = f.data().videoTracks[0].clips[1].id;
        QString error;
        const auto restored = f.project.flattenMulticamClip(seg, &error);
        QVERIFY2(restored, qPrintable(error));
        QCOMPARE(restored->size(), 2);
        const ClipData& v = f.data().videoTracks[0].clips[1];
        QCOMPARE(v.kind, ClipKind::Media);
        QCOMPARE(v.mediaId, QStringLiteral("b"));
        QCOMPARE(v.start, int64_t(100));
        const ClipData& a = f.data().audioTracks[0].clips[1];
        QCOMPARE(a.mediaId, QStringLiteral("b"));
        QVERIFY(v.linkGroup != 0 && v.linkGroup == a.linkGroup);
        // The first segment is still a multicam clip.
        QCOMPARE(f.data().videoTracks[0].clips[0].kind, ClipKind::Nested);
        KK_VERIFY_INVARIANTS(f.data());
        f.project.undo();
        QCOMPARE(f.data().videoTracks[0].clips[1].kind, ClipKind::Nested);
        // A media clip cannot be flattened.
        TimelineData plain = timeline();
        ops::overwriteClip(plain, kV1, mediaClip(0, 10));
        QVERIFY(!mc::flatten(plain, {plain.videoTracks[0].clips[0].id}, lk, &error));
    }

    void saveLoadAndMigration()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("mc.kkproj"));
        {
            Fixture f;
            f.project.beginEdit(QStringLiteral("Cut"));
            mc::cutToAngle(f.project.timeline()->mutableData(), 60, 2, mc::AngleTarget::Audio,
                f.project.timelineLookup());
            f.project.endEdit();
            MediaItem item;
            item.name = QStringLiteral("tc.mov");
            item.info = info(5);
            item.info.startTimecode = QStringLiteral("01:02:03:04");
            f.project.beginEdit(QStringLiteral("Add"));
            f.project.mediaPool()->addItem(item);
            f.project.endEdit();
            QVERIFY(f.project.save(path));

            Project q;
            QString error;
            QVERIFY2(q.load(path, &error), qPrintable(error));
            for (const quint64 uid : f.project.timelineUids())
                QCOMPARE(*q.timelineData(uid), *f.project.timelineData(uid));
            QCOMPARE(q.mediaPool()->state(), f.project.mediaPool()->state());
            QVERIFY(q.timelineData(f.mcUid)->multicam);
            QCOMPARE(
                q.mediaPool()->item(q.timelineItemId(f.mcUid))->kind, MediaItemKind::MulticamClip);
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QCOMPARE(
            root.value(QStringLiteral("formatVersion")).toInt(), serialization::kFormatVersion);

        // A version-5 file (before multicam): nested clips have no angle and show their whole
        // timeline, compound timelines stay compound clips.
        Project p;
        const quint64 cc = p.createCompoundClip({}, QString());
        QVERIFY(!cc); // nothing selected: refused
        p.beginEdit(QStringLiteral("Setup"));
        ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 10));
        p.endEdit();
        QVERIFY(p.createCompoundClip({p.timeline()->data().videoTracks[0].clips[0].id}, {}));
        const QString oldPath = dir.filePath(QStringLiteral("v5.kkproj"));
        QVERIFY(p.save(oldPath));
        QFile old(oldPath);
        QVERIFY(old.open(QIODevice::ReadOnly));
        QJsonObject v5 = QJsonDocument::fromJson(old.readAll()).object();
        old.close();
        v5[QStringLiteral("formatVersion")] = 5;
        QVERIFY(old.open(QIODevice::WriteOnly | QIODevice::Truncate));
        old.write(QJsonDocument(v5).toJson());
        old.close();
        Project q;
        QString error;
        QVERIFY2(q.load(oldPath, &error), qPrintable(error));
        const ClipData& nested = q.timeline()->data().videoTracks[0].clips[0];
        QCOMPARE(nested.kind, ClipKind::Nested);
        QCOMPARE(nested.multicamAngle, -1);
        QVERIFY(!q.timelineData(nested.nestedUid)->multicam);
        QCOMPARE(q.mediaPool()->item(q.timelineItemId(nested.nestedUid))->kind,
            MediaItemKind::CompoundClip);

        // A newer file is refused.
        v5[QStringLiteral("formatVersion")] = serialization::kFormatVersion + 1;
        QVERIFY(old.open(QIODevice::WriteOnly | QIODevice::Truncate));
        old.write(QJsonDocument(v5).toJson());
        old.close();
        QVERIFY(!q.load(oldPath, &error));
    }

    void clipJsonRoundTrip()
    {
        ClipData c;
        c.id = 4;
        c.kind = ClipKind::Nested;
        c.nestedUid = 3;
        c.multicamAngle = 5;
        c.start = 1;
        c.duration = 2;
        QCOMPARE(serialization::clipFromJson(serialization::clipToJson(c)), c);
        c.multicamAngle = -1;
        QVERIFY(!serialization::clipToJson(c).contains(QStringLiteral("multicamAngle")));
    }
};

QTEST_GUILESS_MAIN(TestMulticam)
#include "test_multicam.moc"
