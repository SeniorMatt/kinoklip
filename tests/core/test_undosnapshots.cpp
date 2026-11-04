// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for core/UndoSnapshots: packed undo snapshots round-trip exactly, share unchanged
// clips with the neighbouring step, and the Project's undo / redo still restores every state.
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/UndoSnapshots.h"

#include <QTest>
#include <QUndoStack>

using namespace kk;
namespace ops = editops;

namespace {

TimelineData sample(int clipsPerTrack)
{
    TimelineData d = makeDefaultTimeline(QStringLiteral("T"), Rational {25, 1}, 1920, 1080, 2, 2);
    for (TrackData* t : {&d.videoTracks[0], &d.videoTracks[1], &d.audioTracks[0]}) {
        for (int i = 0; i < clipsPerTrack; ++i) {
            ClipData c;
            c.id = d.allocateId();
            c.kind = ClipKind::Solid;
            c.name = QStringLiteral("C%1").arg(i);
            c.start = i * 10;
            c.duration = 10;
            t->clips.push_back(c);
        }
    }
    d.markers.append(Marker {5, QStringLiteral("M")});
    return d;
}

} // namespace

class TestUndoSnapshots : public QObject
{
    Q_OBJECT
private slots:
    void roundTripIsExact()
    {
        auto data = std::make_shared<const TimelineData>(sample(20));
        auto packed = undosnap::pack(data, nullptr);
        QVERIFY(packed);
        QCOMPARE(undosnap::unpack(*packed), data); // still alive: the same object
        const TimelineData copy = *data;
        data.reset(); // the original is gone: unpack builds a copy
        const auto rebuilt = undosnap::unpack(*packed);
        QVERIFY(*rebuilt == copy);
    }

    void unchangedClipsAreShared()
    {
        auto a = std::make_shared<const TimelineData>(sample(50));
        auto pa = undosnap::pack(a, nullptr);
        TimelineData changed = *a;
        changed.videoTracks[1].clips[7].name = QStringLiteral("Renamed");
        auto b = std::make_shared<const TimelineData>(changed);
        auto pb = undosnap::pack(b, pa);
        // Only the renamed clip is new; the shell (no clip changed it) is shared too.
        QCOMPARE(pb->shell, pa->shell);
        int fresh = 0;
        for (int t = 0; t < kTrackTypeCount; ++t) {
            for (size_t i = 0; i < pb->clips[t].size(); ++i) {
                for (size_t k = 0; k < pb->clips[t][i].size(); ++k)
                    fresh += pb->clips[t][i][k] != pa->clips[t][i][k];
            }
        }
        QCOMPARE(fresh, 1);
        QVERIFY(
            undosnap::exclusiveBytes(*pb, pa.get()) < undosnap::exclusiveBytes(*pa, nullptr) / 10);
        // Packing the state a packed snapshot came from returns that snapshot.
        QCOMPARE(undosnap::pack(a, pa), pa);
    }

    void insertedAndRemovedClipsFindTheirNeighbours()
    {
        auto a = std::make_shared<const TimelineData>(sample(30));
        auto pa = undosnap::pack(a, nullptr);
        TimelineData changed = *a;
        auto& clips = changed.videoTracks[0].clips;
        clips.erase(clips.begin() + 3); // a delete shifts every later clip by one index
        auto b = std::make_shared<const TimelineData>(changed);
        auto pb = undosnap::pack(b, pa);
        for (const auto& clip : pb->clips[0][0])
            QVERIFY(std::find(pa->clips[0][0].cbegin(), pa->clips[0][0].cend(), clip)
                != pa->clips[0][0].cend());
        QVERIFY(*undosnap::unpack(*pb) == changed);
    }

    void projectUndoRedoRestoresEveryState()
    {
        Project project;
        project.beginEdit(QStringLiteral("Fill"));
        project.timeline()->mutableData() = sample(40);
        project.timeline()->mutableData().uid = project.activeTimelineUid();
        project.endEdit();
        QVector<TimelineData> states {project.timeline()->data()};
        for (int i = 0; i < 6; ++i) {
            project.beginEdit(QStringLiteral("Step %1").arg(i));
            TimelineData& d = project.timeline()->mutableData();
            if (i % 2 == 0)
                ops::splitAt(d, 5 + i * 20);
            else
                d.audioTracks[0].clips[size_t(i)].name = QStringLiteral("N%1").arg(i);
            project.endEdit();
            states.append(project.timeline()->data());
        }
        for (int i = int(states.size()) - 2; i >= 0; --i) {
            project.undo();
            QVERIFY2(project.timeline()->data() == states[i], qPrintable(QString::number(i)));
        }
        for (int i = 1; i < states.size(); ++i) {
            project.redo();
            QVERIFY2(project.timeline()->data() == states[i], qPrintable(QString::number(i)));
        }
        // Jumping several steps at once (Edit > History) lands on the same states.
        project.undoStack()->setIndex(2);
        QVERIFY(project.timeline()->data() == states[1]);
    }
};

QTEST_MAIN(TestUndoSnapshots)
#include "test_undosnapshots.moc"
