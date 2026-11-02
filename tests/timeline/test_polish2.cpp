// SPDX-License-Identifier: GPL-3.0-or-later
// Second polish round of the Edit page (docs/polish-v3.md, Round 2): Move Track Up / Down
// (spec 12.9), track height presets (12.10) and the Timeline View Options (12.11), driven
// through the controller and the widget.
#include "TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {

using Preset = TimelineController::TrackHeightPreset;

/// Adds a fake 10 s video media item (no file needed) and returns its id.
QString addMedia(Project& project)
{
    MediaItem item;
    item.path = QStringLiteral("/nonexistent/clip.mp4");
    item.name = QStringLiteral("clip.mp4");
    item.info.valid = true;
    item.info.hasVideo = true;
    item.info.durationUs = 10'000'000;
    item.info.width = 1920;
    item.info.height = 1080;
    return project.mediaPool()->addItem(item);
}

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
        w.resize(1200, 500);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(10.0, kHeaderWidth);
        w.scrollToFrame(0);
    }
    quint64 put(TrackRef ref, ClipData clip)
    {
        f.project.beginEdit(QStringLiteral("setup"));
        const quint64 id = ops::overwriteClip(f.data(), ref, clip);
        f.project.endEdit();
        return id;
    }
    void addTracks(TrackType type, int n)
    {
        f.project.beginEdit(QStringLiteral("setup"));
        for (int i = 0; i < n; ++i)
            ops::addTrack(f.data(), type);
        f.project.endEdit();
    }
    int undoCount() const { return f.project.undoStack()->count(); }
    QString undoText() const { return f.project.undoStack()->undoText(); }
};

} // namespace

class TestPolish2 : public QObject
{
    Q_OBJECT

private slots:
    /// Move Track Up on V1 swaps it with V2 (video numbers grow upwards): its clips, name and
    /// flags go along, the destination patch follows; one undo step; the top track cannot
    /// move further up.
    void spec12_9_moveVideoTrackUp()
    {
        View v;
        v.addTracks(TrackType::Video, 1);
        v.put({TrackType::Video, 0}, solidClip(0, 10, QStringLiteral("low")));
        v.f.project.beginEdit(QStringLiteral("setup"));
        v.f.data().videoTracks[0].name = QStringLiteral("Lower");
        v.f.data().videoTracks[0].height = 100;
        v.f.project.endEdit();
        QCOMPARE(v.c.targetVideoTrack(), 0);
        const int before = v.undoCount();
        QVERIFY(v.c.moveTrack({TrackType::Video, 0}, 1));
        const TimelineData& d = v.f.view();
        QCOMPARE(d.videoTracks[1].name, QStringLiteral("Lower"));
        QCOMPARE(d.videoTracks[1].height, 100);
        QCOMPARE(d.videoTracks[1].clips.size(), size_t(1));
        QVERIFY(d.videoTracks[0].clips.empty());
        QCOMPARE(v.c.targetVideoTrack(), 1);
        QCOMPARE(v.undoCount(), before + 1);
        QCOMPARE(v.undoText(), QStringLiteral("Move Track Up"));
        const int top = int(v.f.view().videoTracks.size()) - 1;
        QVERIFY(!v.c.moveTrack({TrackType::Video, top}, 1)); // already on top
        QCOMPARE(v.undoCount(), before + 1);
        v.f.project.undoStack()->undo();
        QCOMPARE(v.f.view().videoTracks[0].name, QStringLiteral("Lower"));
    }

    /// Audio numbers grow downwards on screen: Move Track Down on A1 makes it A2.
    void spec12_9_moveAudioTrackDown()
    {
        View v;
        v.addTracks(TrackType::Audio, 2);
        ClipData a = solidClip(0, 10, QStringLiteral("a"));
        v.put({TrackType::Audio, 0}, a);
        QVERIFY(v.c.moveTrack({TrackType::Audio, 0}, -1));
        QVERIFY(v.f.view().audioTracks[0].clips.empty());
        QCOMPARE(v.f.view().audioTracks[1].clips.size(), size_t(1));
        QCOMPARE(v.undoText(), QStringLiteral("Move Track Down"));
        QVERIFY(!v.c.moveTrack({TrackType::Audio, 0}, 1)); // A1 is the top audio track
        const int bottom = int(v.f.view().audioTracks.size()) - 1;
        QVERIFY(!v.c.moveTrack({TrackType::Audio, bottom}, -1)); // the bottom audio track
    }

    /// Track height presets: Small / Medium / Large on one track (the row height follows,
    /// one undo step "Track Height"), Medium is the default height, and the timeline-wide
    /// variant sets every track in one step.
    void spec12_10_trackHeightPresets()
    {
        View v;
        v.addTracks(TrackType::Video, 1);
        const TrackRef v1 {TrackType::Video, 0};
        QCOMPARE(TimelineController::heightPresetOf(v.f.view().videoTracks[0]), Preset::Medium);
        const int medium = v.w.layout().rowRect(v.w.layout().rowOf(v1)).height();
        const int before = v.undoCount();
        QVERIFY(v.c.setTrackHeightPreset(v1, Preset::Large));
        QCOMPARE(v.undoText(), QStringLiteral("Track Height"));
        QCoreApplication::processEvents();
        const int large = v.w.layout().rowRect(v.w.layout().rowOf(v1)).height();
        QVERIFY2(large > medium, qPrintable(QStringLiteral("%1 %2").arg(large).arg(medium)));
        QCOMPARE(TimelineController::heightPresetOf(v.f.view().videoTracks[0]), Preset::Large);
        QVERIFY(!v.c.setTrackHeightPreset(v1, Preset::Large)); // no change: no step
        QCOMPARE(v.undoCount(), before + 1);
        QVERIFY(v.c.setTrackHeightPreset(v1, Preset::Small, true));
        QCOMPARE(v.undoCount(), before + 2);
        for (const TrackData& t : v.f.view().videoTracks)
            QCOMPARE(TimelineController::heightPresetOf(t), Preset::Small);
        for (const TrackData& t : v.f.view().audioTracks)
            QCOMPARE(TimelineController::heightPresetOf(t), Preset::Small);
        QVERIFY(
            TimelineController::presetHeight(TrackType::Video, Preset::Small) >= kMinTrackHeight);
        // The header menu offers the three presets with the current one checked.
        QMenu menu;
        v.w.addTrackHeightMenu(menu, v1, false);
        QMenu* sub = menu.findChild<QMenu*>(QStringLiteral("TrackHeightMenu"));
        QVERIFY(sub);
        QCOMPARE(sub->actions().size(), 3);
        QVERIFY(sub->actions()[0]->isChecked());
        sub->actions()[2]->trigger();
        QCOMPARE(TimelineController::heightPresetOf(v.f.view().videoTracks[0]), Preset::Large);
    }

    /// Timeline View Options: filmstrip asks for a thumbnail per tile, head-and-tail for two
    /// per clip, clip-name-only for none; waveforms can be hidden.
    void spec12_11_viewOptionsThumbnails()
    {
        View v;
        const QString media = addMedia(v.f.project);
        ClipData c;
        c.kind = ClipKind::Media;
        c.mediaId = media;
        c.name = QStringLiteral("clip");
        c.start = 0;
        c.duration = 100; // 1000 px at 10 px per frame
        c.sourceDurationUs = 10'000'000;
        v.put({TrackType::Video, 0}, c);
        int requests = 0;
        v.w.setThumbnailProvider([&](const QString&, int64_t, int) {
            ++requests;
            return QImage();
        });
        auto paintCount = [&](ClipThumbnails mode) {
            v.w.setClipThumbnails(mode);
            requests = 0;
            v.w.viewport()->repaint();
            return requests;
        };
        const int film = paintCount(ClipThumbnails::Filmstrip);
        QVERIFY2(film > 4, qPrintable(QString::number(film)));
        QCOMPARE(paintCount(ClipThumbnails::HeadTail), 2);
        QCOMPARE(paintCount(ClipThumbnails::None), 0);
        QVERIFY(v.w.showWaveforms());
        v.w.setShowWaveforms(false);
        QVERIFY(!v.w.showWaveforms());
    }

    /// Edit > Undo names the action (round 2 audit U1): header buttons say what they did.
    void undoNamesTrackButtons()
    {
        View v;
        const TrackRef v1 {TrackType::Video, 0};
        const TrackRef a1 {TrackType::Audio, 0};
        v.c.toggleTrackFlag(v1, TimelineController::FlagLocked);
        QCOMPARE(v.undoText(), QStringLiteral("Lock Track"));
        v.c.toggleTrackFlag(v1, TimelineController::FlagLocked);
        QCOMPARE(v.undoText(), QStringLiteral("Unlock Track"));
        v.c.toggleTrackFlag(a1, TimelineController::FlagMuted);
        QCOMPARE(v.undoText(), QStringLiteral("Mute Track"));
        v.c.toggleTrackFlag(a1, TimelineController::FlagSolo);
        QCOMPARE(v.undoText(), QStringLiteral("Solo Track"));
        v.c.toggleTrackFlag(v1, TimelineController::FlagEnabled);
        QCOMPARE(v.undoText(), QStringLiteral("Disable Track"));
        v.c.toggleTrackFlag(v1, TimelineController::FlagAutoSelect);
        QCOMPARE(v.undoText(), QStringLiteral("Auto Track Selector"));
        // Several media pool items dropped at once: named after the edit.
        const QString media = addMedia(v.f.project);
        v.c.editSources({SourceSelection {media, 0, 0}, SourceSelection {media, 0, 0}},
            SourceEditMode::AppendAtEnd);
        QCOMPARE(v.undoText(), QStringLiteral("Append at End"));
    }
};

QTEST_MAIN(TestPolish2)
#include "test_polish2.moc"
