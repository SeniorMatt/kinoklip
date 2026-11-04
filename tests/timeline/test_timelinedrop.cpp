// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen drag-and-drop tests of the timeline as a drop target: every payload (media items,
// source range, effect, files from a file manager in each format Wayland sources use), the
// empty track area (new video track above, new audio track below, the V/A divider), refused
// areas, and the explicit DragMove answers Wayland needs. The real compositor path is covered
// by tests/app/test_wayland.cpp.
#include "TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "ui/common/DragMime.h"
#include "ui/common/MediaImport.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>
#include <QUrl>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kV2 {TrackType::Video, 1};
constexpr TrackRef kA1 {TrackType::Audio, 0};
constexpr TrackRef kA2 {TrackType::Audio, 1};

#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
using DragPos = QPointF; // Qt 6.12 deprecates the QPoint constructors of the drag events
#else
using DragPos = QPoint; // Qt 6.4 has no QPointF ones
#endif

struct Answers
{
    bool enter = false;
    bool move = false;
    bool drop = false;
};

/// Sends DragEnter, DragMove and Drop of @p mime at @p pos to @p target like Qt does for a
/// real drag (each event starts unaccepted; Qt Wayland forwards each answer to the compositor).
/// drop is only meaningful when move was accepted.
Answers sendDrag(
    QWidget* target, const QMimeData* mime, QPoint pos, Qt::KeyboardModifiers mods = {})
{
    Answers a;
    QDragEnterEvent enter(DragPos(pos), Qt::CopyAction, mime, Qt::LeftButton, mods);
    enter.setAccepted(false);
    QCoreApplication::sendEvent(target, &enter);
    a.enter = enter.isAccepted();
    QDragMoveEvent move(DragPos(pos), Qt::CopyAction, mime, Qt::LeftButton, mods);
    move.setAccepted(false);
    QCoreApplication::sendEvent(target, &move);
    a.move = move.isAccepted();
    // Always end with a drop: QApplication remembers the accepting widget as the drag target
    // until a Drop, and a later test's events would reach the deleted widget otherwise. A
    // refused drop is ignored by every target.
    QDropEvent drop(DragPos(pos), Qt::CopyAction, mime, Qt::LeftButton, mods);
    drop.setAccepted(false);
    QCoreApplication::sendEvent(target, &drop);
    a.drop = drop.isAccepted();
    return a;
}

struct View
{
    TimelineFixture f;
    TimelineController c {f.ctx};
    TimelineWidget w {f.ctx, &c};
    QString mediaId;

    View()
    {
        c.connectActions();
        c.setSnapping(false);
        w.setThumbnailProvider({});
        w.setWaveformProvider({});
        w.resize(1200, 700); // tall: empty space above V2 and below A2
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(10.0, kHeaderWidth);
        w.scrollToFrame(0);
        MediaItem item;
        item.path = QStringLiteral("/nonexistent/a.mp4");
        item.name = QStringLiteral("a.mp4");
        item.info.valid = item.info.hasVideo = item.info.hasAudio = true;
        item.info.durationUs = 2'000'000; // 50 frames at 25 fps
        mediaId = f.project.mediaPool()->addItem(item);
    }
    QWidget* vp() { return w.viewport(); }
    int x(double frame) const { return int(std::lround(w.layout().frameToX(frame))); }
    QPoint at(double frame, TrackRef ref) const
    {
        const TimelineLayout& L = w.layout();
        return QPoint(x(frame), L.rowRect(L.rowOf(ref)).center().y());
    }
    QRect rowRect(TrackRef ref) const { return w.layout().rowRect(w.layout().rowOf(ref)); }
    std::unique_ptr<QMimeData> mediaMime() const
    {
        auto mime = std::make_unique<QMimeData>();
        mime->setData(QLatin1String(mime::MediaItems), mediaId.toUtf8());
        return mime;
    }
    int clipCount(TrackRef ref) const
    {
        const TrackData* t = f.view().track(ref);
        return t ? int(t->clips.size()) : -1;
    }
};

/// Writes a small PNG (importable still) into @p dir.
QString writeStill(const QTemporaryDir& dir, const QString& name)
{
    QImage image(64, 36, QImage::Format_RGB32);
    image.fill(Qt::green);
    const QString path = dir.filePath(name);
    image.save(path);
    return path;
}

/// Shared generated test media (tests/data/generated).
QString testFile(const char* name)
{
    return QStringLiteral(KK_TEST_DATA_DIR "/generated/") + QLatin1String(name);
}

/// DragEnter + DragMove of @p mime at @p pos, without the drop (the ghost stays).
bool hover(QWidget* target, const QMimeData* mime, QPoint pos, Qt::KeyboardModifiers mods = {})
{
    QDragEnterEvent enter(DragPos(pos), Qt::CopyAction, mime, Qt::LeftButton, mods);
    enter.setAccepted(false);
    QCoreApplication::sendEvent(target, &enter);
    QDragMoveEvent move(DragPos(pos), Qt::CopyAction, mime, Qt::LeftButton, mods);
    move.setAccepted(false);
    QCoreApplication::sendEvent(target, &move);
    return enter.isAccepted() && move.isAccepted();
}

/// Ends a hover() with a drop at @p pos; returns whether it was accepted.
bool drop(QWidget* target, const QMimeData* mime, QPoint pos, Qt::KeyboardModifiers mods = {})
{
    QDropEvent event(DragPos(pos), Qt::CopyAction, mime, Qt::LeftButton, mods);
    event.setAccepted(false);
    QCoreApplication::sendEvent(target, &event);
    return event.isAccepted();
}

std::unique_ptr<QMimeData> fileMime(const QStringList& paths)
{
    auto mime = std::make_unique<QMimeData>();
    QList<QUrl> urls;
    for (const QString& p : paths)
        urls.append(QUrl::fromLocalFile(p));
    mime->setUrls(urls);
    return mime;
}

/// Closes every modal dialog (the import report) that opens in the next @p ms; counts them.
struct ModalCloser
{
    int closed = 0;
    QTimer timer;
    explicit ModalCloser()
    {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            if (QWidget* w = QApplication::activeModalWidget()) {
                ++closed;
                w->close();
            }
        });
        timer.start();
    }
};

} // namespace

class TestTimelineDrop : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_timelinedrop"); }

    void mediaItemsOnTrackAndMoveAnswered()
    {
        View v;
        const auto mime = v.mediaMime();
        const Answers a = sendDrag(v.vp(), mime.get(), v.at(10, kV1));
        QVERIFY(a.enter);
        QVERIFY(a.move); // every DragMove is answered explicitly
        QVERIFY(a.drop);
        QCOMPARE(v.clipCount(kV1), 1);
        QCOMPARE(v.clipCount(kA1), 1);
        QCOMPARE(v.f.view().videoTracks[0].clips[0].start, int64_t(10));
    }

    void sourceRangeDrop()
    {
        View v;
        QJsonObject json;
        json.insert(QStringLiteral("mediaId"), v.mediaId);
        json.insert(QStringLiteral("inUs"), 400'000.0); // frames 10..30
        json.insert(QStringLiteral("outUs"), 1'200'000.0);
        QMimeData mime;
        mime.setData(
            QLatin1String(mime::SourceRange), QJsonDocument(json).toJson(QJsonDocument::Compact));
        QVERIFY(sendDrag(v.vp(), &mime, v.at(5, kV2)).drop);
        QCOMPARE(v.clipCount(kV2), 1);
        QCOMPARE(v.f.view().videoTracks[1].clips[0].duration, int64_t(20));
        QCOMPARE(v.clipCount(kA2), 1); // same index pairing
    }

    void emptySpaceAboveCreatesVideoTrack()
    {
        View v;
        const QRect top = v.rowRect(kV2);
        QVERIFY2(top.top() > kRulerHeight + 20, "the test needs empty space above V2");
        const auto mime = v.mediaMime();
        const QPoint above(v.x(0), top.top() - 10);
        const int undoBefore = v.f.project.undoStack()->count();
        QVERIFY(sendDrag(v.vp(), mime.get(), above).drop);
        QCOMPARE(int(v.f.view().videoTracks.size()), 3);
        QCOMPARE(v.clipCount(TrackRef {TrackType::Video, 2}), 1);
        // The linked audio goes to the same index, created too (V3 pairs with A3).
        QCOMPARE(int(v.f.view().audioTracks.size()), 3);
        QCOMPARE(v.clipCount(TrackRef {TrackType::Audio, 2}), 1);
        QCOMPARE(v.f.project.undoStack()->count(), undoBefore + 1); // one undo step
        v.f.project.undoStack()->undo();
        QCOMPARE(int(v.f.view().videoTracks.size()), 2);
        KK_VERIFY_TIMELINE(v.f.view());
    }

    void emptySpaceBelowCreatesAudioTrack()
    {
        View v;
        const QRect bottom = v.rowRect(kA2);
        QVERIFY2(bottom.bottom() < v.vp()->height() - 20, "the test needs space below A2");
        const auto mime = v.mediaMime();
        QVERIFY(sendDrag(v.vp(), mime.get(), QPoint(v.x(0), bottom.bottom() + 10)).drop);
        QCOMPARE(int(v.f.view().audioTracks.size()), 3);
        QCOMPARE(v.clipCount(TrackRef {TrackType::Audio, 2}), 1);
        QCOMPARE(int(v.f.view().videoTracks.size()), 3);
        QCOMPARE(v.clipCount(TrackRef {TrackType::Video, 2}), 1);
        KK_VERIFY_TIMELINE(v.f.view());
    }

    void titleAboveCreatesVideoTrackButNotBelow()
    {
        View v;
        QMimeData title;
        title.setData(QLatin1String(mime::Effect), "text_title");
        QVERIFY(sendDrag(v.vp(), &title, QPoint(v.x(0), v.rowRect(kV2).top() - 10)).drop);
        QCOMPARE(int(v.f.view().videoTracks.size()), 3);
        QCOMPARE(int(v.f.view().audioTracks.size()), 2); // a title has no audio
        const Answers below
            = sendDrag(v.vp(), &title, QPoint(v.x(0), v.rowRect(kA2).bottom() + 10));
        QVERIFY(!below.move);
        QCOMPARE(int(v.f.view().audioTracks.size()), 2);
    }

    // Spec 5.5: a drop in the empty space creates only the tracks its clips land on: a
    // video-only clip above the top video track makes a new video track and no empty audio
    // track, an audio-only clip below the last audio track no empty video track.
    void spec5_5_videoOnlyAboveCreatesNoAudioTrack()
    {
        View v;
        MediaItem videoOnly;
        videoOnly.path = QStringLiteral("/nonexistent/silent.mp4");
        videoOnly.name = QStringLiteral("silent.mp4");
        videoOnly.info.valid = videoOnly.info.hasVideo = true;
        videoOnly.info.durationUs = 2'000'000;
        const QString videoId = v.f.project.mediaPool()->addItem(videoOnly);
        QMimeData mime;
        mime.setData(QLatin1String(mime::MediaItems), videoId.toUtf8());
        QVERIFY(sendDrag(v.vp(), &mime, QPoint(v.x(0), v.rowRect(kV2).top() - 10)).drop);
        QCOMPARE(int(v.f.view().videoTracks.size()), 3);
        QCOMPARE(v.clipCount(TrackRef {TrackType::Video, 2}), 1);
        QCOMPARE(int(v.f.view().audioTracks.size()), 2);
        KK_VERIFY_TIMELINE(v.f.view());
    }
    void spec5_5_audioOnlyBelowCreatesNoVideoTrack()
    {
        View v;
        MediaItem audioOnly;
        audioOnly.path = QStringLiteral("/nonexistent/music.wav");
        audioOnly.name = QStringLiteral("music.wav");
        audioOnly.info.valid = audioOnly.info.hasAudio = true;
        audioOnly.info.durationUs = 2'000'000;
        const QString audioId = v.f.project.mediaPool()->addItem(audioOnly);
        QMimeData music;
        music.setData(QLatin1String(mime::MediaItems), audioId.toUtf8());
        QVERIFY(sendDrag(v.vp(), &music, QPoint(v.x(0), v.rowRect(kA2).bottom() + 10)).drop);
        QCOMPARE(int(v.f.view().audioTracks.size()), 3);
        QCOMPARE(v.clipCount(TrackRef {TrackType::Audio, 2}), 1);
        QCOMPARE(int(v.f.view().videoTracks.size()), 2);
        KK_VERIFY_TIMELINE(v.f.view());
    }

    void dividerCountsAsNearestRow()
    {
        View v;
        const QRect v1 = v.rowRect(kV1);
        const QRect a1 = v.rowRect(kA1);
        if (a1.top() - v1.bottom() < 3)
            QSKIP("no V/A divider gap in this layout");
        const auto mime = v.mediaMime();
        QVERIFY(sendDrag(v.vp(), mime.get(), QPoint(v.x(0), v1.bottom() + 1)).drop);
        QCOMPARE(v.clipCount(kV1), 1);
    }

    void rulerAndHeadersRefuse()
    {
        View v;
        const auto mime = v.mediaMime();
        const Answers ruler = sendDrag(v.vp(), mime.get(), QPoint(v.x(10), kRulerHeight / 2));
        QVERIFY(ruler.enter); // our format: the timeline takes the drag ...
        QVERIFY(!ruler.move); // ... but no drop there (the cursor shows "forbidden")
        const Answers header
            = sendDrag(v.vp(), mime.get(), QPoint(kHeaderWidth / 2, v.at(0, kV1).y()));
        QVERIFY(!header.move);
        QCOMPARE(v.clipCount(kV1), 0);
    }

    void foreignFormatIgnored()
    {
        View v;
        QMimeData text;
        text.setText(QStringLiteral("hello"));
        const Answers a = sendDrag(v.vp(), &text, v.at(10, kV1));
        QVERIFY(!a.enter);
    }

    void effectAndTransitionOnClip()
    {
        View v;
        ClipData c = solidClip(0, 50);
        v.f.project.beginEdit(QStringLiteral("setup"));
        const quint64 id = ops::overwriteClip(v.f.data(), kV1, c);
        v.f.project.endEdit();
        QMimeData effect;
        effect.setData(QLatin1String(mime::Effect), "gaussian_blur");
        QVERIFY(sendDrag(v.vp(), &effect, v.at(20, kV1)).drop);
        QCOMPARE(ops::clipById(v.f.view(), id)->effects.size(), 1);
        // Not over a clip: refused, no change.
        QVERIFY(!sendDrag(v.vp(), &effect, v.at(80, kV1)).move);
        QMimeData transition;
        transition.setData(QLatin1String(mime::Effect), "cross_dissolve");
        QVERIFY(sendDrag(v.vp(), &transition, v.at(45, kV1)).drop);
        QVERIFY(ops::clipById(v.f.view(), id)->transitionOut);
    }

    void filesFromFileManager_data()
    {
        QTest::addColumn<QString>("format");
        QTest::newRow("text/uri-list") << QStringLiteral("text/uri-list");
        // Dolphin / Plasma also offer the list as application/x-kde4-urilist; Qt Wayland loses
        // text/uri-list when the document portal cannot resolve the key it also offers.
        QTest::newRow("application/x-kde4-urilist") << QStringLiteral("application/x-kde4-urilist");
    }

    void filesFromFileManager()
    {
        QFETCH(QString, format);
        View v;
        QTemporaryDir dir;
        const QString path = writeStill(dir, QStringLiteral("still.png"));
        QMimeData mime;
        const QByteArray list = QUrl::fromLocalFile(path).toEncoded() + "\r\n";
        if (format == QLatin1String("text/uri-list"))
            mime.setUrls({QUrl::fromLocalFile(path)});
        else
            mime.setData(format, list);
        const auto itemsBefore = v.f.project.mediaPool()->items().size(); // includes the timeline
        const Answers a = sendDrag(v.vp(), &mime, v.at(10, kV2));
        QVERIFY(a.enter);
        QVERIFY(a.move);
        QVERIFY(a.drop);
        // Imported and placed from the event loop (no modal dialog inside the drop).
        QTRY_COMPARE(v.f.project.mediaPool()->items().size(), itemsBefore + 1);
        QTRY_COMPARE(v.clipCount(kV2), 1);
        QCOMPARE(v.f.view().videoTracks[1].clips[0].start, int64_t(10));
        QCOMPARE(v.clipCount(kA2), 0); // a still has no audio
    }

    /// Spec 5.6: files dragged in from a file manager show the ghost of a media pool drag
    /// while the drag goes on: the real length (probed on a worker), the video on the track
    /// under the cursor and its audio on the matching audio track. The drop imports into the
    /// media pool's current bin and places the clips where the ghost was; one Undo takes them
    /// off the timeline and keeps the pool item, as after importing and dragging from the pool.
    void spec5_6_fileGhostShowsTheRealLengthAndDropsThere()
    {
        const QString video = testFile("h264_25fps.mp4"); // 10 s with audio
        if (!QFileInfo::exists(video))
            QSKIP("test media missing (tests/data/generated)");
        View v;
        MediaPool* pool = v.f.project.mediaPool();
        const QString bin = pool->addBin(QStringLiteral("Shoot"));
        v.c.setBinProvider([bin] { return bin; });
        const auto mime = fileMime({video});
        QVERIFY(hover(v.vp(), mime.get(), v.at(10, kV2)));
        // A placeholder at once (the drag never waits), the real length when probed.
        QVERIFY(!v.w.dropGhosts().empty());
        QTRY_COMPARE(int(v.w.dropGhosts().size()), 2);
        QTRY_COMPARE(v.w.dropGhosts()[0].duration, int64_t(250));
        const auto& g = v.w.dropGhosts();
        QCOMPARE(g[0].row, v.w.layout().rowOf(kV2));
        QCOMPARE(g[1].row, v.w.layout().rowOf(kA2));
        QCOMPARE(g[0].start, int64_t(10));
        QCOMPARE(g[1].duration, int64_t(250));
        QCOMPARE(g[0].name, QStringLiteral("h264_25fps.mp4"));
        QCOMPARE(v.clipCount(kV2), 0); // only a preview

        const auto itemsBefore = pool->items().size();
        QVERIFY(drop(v.vp(), mime.get(), v.at(10, kV2)));
        QVERIFY(v.w.dropGhosts().empty());
        QTRY_COMPARE(v.clipCount(kV2), 1);
        QCOMPARE(v.clipCount(kA2), 1);
        const ClipData& c = v.f.view().videoTracks[1].clips[0];
        QCOMPARE(c.start, int64_t(10));
        QCOMPARE(c.duration, int64_t(250));
        QCOMPARE(v.f.view().audioTracks[1].clips[0].start, int64_t(10));
        QVERIFY(c.linkGroup != 0);
        QCOMPARE(pool->items().size(), itemsBefore + 1);
        const MediaItem* item = pool->item(c.mediaId);
        QVERIFY(item);
        QCOMPARE(item->binId, bin);

        v.f.project.undoStack()->undo();
        QCOMPARE(v.clipCount(kV2), 0);
        QCOMPARE(v.clipCount(kA2), 0);
        QCOMPARE(pool->items().size(), itemsBefore + 1); // the import is its own step
    }

    /// Spec 33.44: with Preferences > Editing > "Import new media into" = Master bin, a file
    /// dropped on the timeline goes into Master, not the media pool's current bin.
    void spec33_44_timelineDropFollowsTheImportPreference()
    {
        const QString still = testFile("still_alpha.png");
        if (!QFileInfo::exists(still))
            QSKIP("test media missing (tests/data/generated)");
        View v;
        MediaPool* pool = v.f.project.mediaPool();
        const QString bin = pool->addBin(QStringLiteral("Shoot"));
        v.c.setBinProvider([bin] { return bin; });
        QCOMPARE(v.c.targetBin(), bin);
        prefs::setImportIntoMasterBin(true);
        QCOMPARE(v.c.targetBin(), QString());
        const auto mime = fileMime({still});
        QVERIFY(hover(v.vp(), mime.get(), v.at(5, kV1)));
        QVERIFY(drop(v.vp(), mime.get(), v.at(5, kV1)));
        QTRY_COMPARE(v.clipCount(kV1), 1);
        const MediaItem* item = pool->item(v.f.view().videoTracks[0].clips[0].mediaId);
        QVERIFY(item);
        QCOMPARE(item->binId, QString());
        prefs::setImportIntoMasterBin(false);
    }

    /// Spec 5.6: several files lay out one after another; a still gets the Preferences still
    /// duration, a WAV goes onto the audio track only.
    void spec5_6_twoFilesLayOutOneAfterAnother()
    {
        const QString still = testFile("still_alpha.png");
        const QString wav = testFile("mono_ramp_48k.wav"); // 5 s
        if (!QFileInfo::exists(still) || !QFileInfo::exists(wav))
            QSKIP("test media missing (tests/data/generated)");
        View v;
        const int64_t stillFrames = TimelineController::defaultStillFrames({25, 1});
        const auto mime = fileMime({still, wav});
        QVERIFY(hover(v.vp(), mime.get(), v.at(10, kV1)));
        QTRY_COMPARE(int(v.w.dropGhosts().size()), 2);
        QTRY_COMPARE(v.w.dropGhosts()[1].row, v.w.layout().rowOf(kA1));
        const auto& g = v.w.dropGhosts();
        QCOMPARE(g[0].row, v.w.layout().rowOf(kV1));
        QCOMPARE(g[0].start, int64_t(10));
        QCOMPARE(g[0].duration, stillFrames);
        QCOMPARE(g[1].start, 10 + stillFrames);
        QCOMPARE(g[1].duration, int64_t(125));
        QVERIFY(drop(v.vp(), mime.get(), v.at(10, kV1)));
        QTRY_COMPARE(v.clipCount(kA1), 1);
        QCOMPARE(v.clipCount(kV1), 1);
        QCOMPARE(v.f.view().videoTracks[0].clips[0].duration, stillFrames);
        QCOMPARE(v.f.view().audioTracks[0].clips[0].start, 10 + stillFrames);
        QCOMPARE(v.f.view().audioTracks[0].clips[0].duration, int64_t(125));
    }

    /// Spec 5.6: the ghost of a file snaps to clip edges like a media pool drag, and the drop
    /// lands on the snapped frame.
    void spec5_6_fileGhostSnapsToAClipEdge()
    {
        const QString still = testFile("still_alpha.png");
        if (!QFileInfo::exists(still))
            QSKIP("test media missing (tests/data/generated)");
        View v;
        v.w.setPixelsPerFrame(2.0, kHeaderWidth); // 8 px snap distance = 4 frames
        v.w.scrollToFrame(0);
        v.f.project.beginEdit(QStringLiteral("setup"));
        ops::overwriteClip(v.f.data(), kV1, solidClip(0, 40));
        v.f.project.endEdit();
        v.c.setSnapping(true);
        const auto mime = fileMime({still});
        QVERIFY(hover(v.vp(), mime.get(), v.at(42, kV2)));
        QTRY_COMPARE(v.w.dropGhosts().size(), size_t(1));
        QCOMPARE(v.w.dropGhosts()[0].start, int64_t(40));
        QVERIFY(drop(v.vp(), mime.get(), v.at(42, kV2)));
        QTRY_COMPARE(v.clipCount(kV2), 1);
        QCOMPARE(v.f.view().videoTracks[1].clips[0].start, int64_t(40));
    }

    /// Spec 5.6: a file that cannot be decoded shows no ghost; the drop places nothing and
    /// reports it like any import (the "could not be imported" message).
    void spec5_6_unsupportedFileShowsNoGhost()
    {
        View v;
        QTemporaryDir dir;
        const QString bad = dir.filePath(QStringLiteral("notes.mp4"));
        {
            QFile f(bad);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("this is not a video");
        }
        const auto mime = fileMime({bad});
        hover(v.vp(), mime.get(), v.at(10, kV1));
        QTRY_VERIFY(v.w.dropGhosts().empty()); // the placeholder goes once the probe fails
        QTest::qWait(100);
        QVERIFY(v.w.dropGhosts().empty());
        const auto itemsBefore = v.f.project.mediaPool()->items().size();
        ModalCloser closer;
        drop(v.vp(), mime.get(), v.at(10, kV1));
        QTRY_COMPARE(closer.closed, 1);
        QCOMPARE(v.clipCount(kV1), 0);
        QCOMPARE(v.f.project.mediaPool()->items().size(), itemsBefore);
    }

    /// Spec 5.6: a file already in the media pool is not imported again: its pool item is
    /// placed, with no "already in the media pool" report.
    void spec5_6_fileAlreadyInThePoolIsReused()
    {
        const QString still = testFile("still_alpha.png");
        if (!QFileInfo::exists(still))
            QSKIP("test media missing (tests/data/generated)");
        View v;
        const QStringList ids = v.f.project.importMedia({still}).importedIds;
        QCOMPARE(ids.size(), 1);
        const auto itemsBefore = v.f.project.mediaPool()->items().size();
        const auto mime = fileMime({still});
        QVERIFY(hover(v.vp(), mime.get(), v.at(5, kV1)));
        QCOMPARE(v.w.dropGhosts().size(), size_t(1)); // known at once: no probe needed
        ModalCloser closer;
        QVERIFY(drop(v.vp(), mime.get(), v.at(5, kV1)));
        QTRY_COMPARE(v.clipCount(kV1), 1);
        QCOMPARE(v.f.view().videoTracks[0].clips[0].mediaId, ids.front());
        QCOMPARE(v.f.project.mediaPool()->items().size(), itemsBefore);
        QTest::qWait(50);
        QCOMPARE(closer.closed, 0);
    }

    /// Spec 5.6: leaving the timeline (or Escape in the drag, which Qt sends as a leave)
    /// removes the ghost; nothing is imported.
    void spec5_6_leavingRemovesTheGhost()
    {
        const QString still = testFile("still_alpha.png");
        if (!QFileInfo::exists(still))
            QSKIP("test media missing (tests/data/generated)");
        View v;
        const auto mime = fileMime({still});
        QVERIFY(hover(v.vp(), mime.get(), v.at(5, kV1)));
        QTRY_COMPARE(v.w.dropGhosts().size(), size_t(1));
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(v.vp(), &leave);
        QVERIFY(v.w.dropGhosts().empty());
        QTest::qWait(100); // a late probe answer does not bring it back
        QVERIFY(v.w.dropGhosts().empty());
        const auto itemsBefore = v.f.project.mediaPool()->items().size();
        QMimeData nothing; // end the drag for QApplication with a refused drop
        drop(v.vp(), &nothing, v.at(5, kV1));
        QCOMPARE(v.f.project.mediaPool()->items().size(), itemsBefore);
    }

    /// Spec 5.6 with 25.18: the project's first import by a drop on the timeline still asks
    /// to change the project frame rate, and the clip is placed after the answer.
    void spec5_6_firstImportStillAsksTheFrameRate()
    {
        const QString video = testFile("h264_2997.mp4");
        if (!QFileInfo::exists(video))
            QSKIP("test media missing (tests/data/generated)");
        View v;
        v.f.project.mediaPool()->removeItem(v.mediaId); // an empty pool: the first import
        int asked = 0;
        setFrameRatePromptHandlerForTests([&](Rational, Rational) {
            ++asked;
            return true;
        });
        const auto mime = fileMime({video});
        QVERIFY(hover(v.vp(), mime.get(), v.at(10, kV1)));
        QVERIFY(drop(v.vp(), mime.get(), v.at(10, kV1)));
        QTRY_COMPARE(v.clipCount(kV1), 1);
        setFrameRatePromptHandlerForTests({});
        QCOMPARE(asked, 1);
        QCOMPARE(v.f.view().fps, (Rational {30000, 1001}));
        QCOMPARE(v.f.view().videoTracks[0].clips[0].start, int64_t(10));
    }

    void folderGoesToTheMediaPoolOnly()
    {
        // Spec 33.24: a folder dropped on the timeline is imported (bins included) into the
        // media pool, nothing of it is placed on the timeline; a file next to it still is.
        View v;
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Stills/Sub")));
        QTemporaryDir other;
        writeStill(dir, QStringLiteral("Stills/one.png"));
        writeStill(dir, QStringLiteral("Stills/Sub/two.png"));
        const QString loose = writeStill(other, QStringLiteral("loose.png"));
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(dir.filePath(QStringLiteral("Stills"))),
            QUrl::fromLocalFile(loose)});
        const auto itemsBefore = v.f.project.mediaPool()->items().size();
        QVERIFY(sendDrag(v.vp(), &mime, v.at(10, kV2)).drop);
        QTRY_COMPARE(v.f.project.mediaPool()->items().size(), itemsBefore + 3);
        QTRY_COMPARE(v.clipCount(kV2), 1); // only the loose file
        const MediaItem* placed
            = v.f.project.mediaPool()->item(v.f.view().videoTracks[1].clips[0].mediaId);
        QVERIFY(placed);
        QCOMPARE(placed->path, loose);
        QCOMPARE(int(v.f.project.mediaPool()->bins().size()), 2); // Stills, Stills/Sub
    }

    void portalKeyAloneDoesNotCrash()
    {
        View v;
        const auto itemsBefore = v.f.project.mediaPool()->items().size(); // includes the timeline
        QMimeData mime;
        mime.setData(QStringLiteral("application/vnd.portal.filetransfer"), "no-such-key");
        const Answers a = sendDrag(v.vp(), &mime, v.at(10, kV1));
        QVERIFY(a.enter); // may carry files: accepted while dragging ...
        QTest::qWait(50);
        QCOMPARE(v.clipCount(kV1), 0); // ... nothing resolvable: nothing happens
        QCOMPARE(v.f.project.mediaPool()->items().size(), itemsBefore);
    }

    /// A subtitle item from the media pool: one caption per cue on the subtitle tracks
    /// (overlapping cues on a second, new track), timed from the drop point, never linked;
    /// no video / audio track appears although it is dropped in the empty space above.
    void subtitleItemCreatesCaptions()
    {
        View v;
        MediaItem item;
        item.kind = MediaItemKind::Subtitle;
        item.path = QStringLiteral("/nonexistent/c.srt");
        item.cues = {{0, 1'000'000, QStringLiteral("one")},
            {400'000, 1'200'000, QStringLiteral("overlap")},
            {2'000'000, 3'000'000, QStringLiteral("two")}};
        item.info = subtitleInfo(item.path, item.cues);
        const QString id = v.f.project.mediaPool()->addItem(item);
        auto mime = std::make_unique<QMimeData>();
        mime->setData(QLatin1String(mime::MediaItems), id.toUtf8());
        const int videoTracks = int(v.f.view().videoTracks.size());
        const Answers a = sendDrag(v.vp(), mime.get(), v.at(20, kV1));
        QVERIFY(a.move && a.drop);
        const TimelineData& d = v.f.view();
        QCOMPARE(int(d.subtitleTracks.size()), 2);
        QCOMPARE(int(d.videoTracks.size()), videoTracks);
        QCOMPARE(v.clipCount(kV1), 0);
        const auto& st1 = d.subtitleTracks[0].clips;
        QCOMPARE(int(st1.size()), 2);
        QCOMPARE(st1[0].start, int64_t(20));
        QCOMPARE(st1[0].duration, int64_t(25));
        QCOMPARE(st1[1].start, int64_t(70));
        QCOMPARE(st1[0].linkGroup, quint64(0));
        QCOMPARE(int(d.subtitleTracks[1].clips.size()), 1);
        QCOMPARE(d.subtitleTracks[1].clips[0].start, int64_t(30));
        // One undo step removes everything again.
        v.f.project.undoStack()->undo();
        QVERIFY(v.f.view().subtitleTracks.empty());
    }

    /// Timeline > Add Subtitle: at the playhead on ST1 (created), shortened before the next
    /// caption, on the next free track when the playhead is inside one; selected.
    void addSubtitleAtPlayhead()
    {
        View v;
        v.f.project.timeline()->setPlayhead(10);
        const quint64 first = v.c.addSubtitleAtPlayhead();
        QVERIFY(first);
        QCOMPARE(v.f.selection.currentClip(), first);
        QCOMPARE(int(v.f.view().subtitleTracks.size()), 1);
        QCOMPARE(v.f.view().subtitleTracks[0].clips[0].duration, int64_t(75)); // 3 s
        v.f.project.timeline()->setPlayhead(0);
        const quint64 before = v.c.addSubtitleAtPlayhead();
        QCOMPARE(ops::clipById(v.f.data(), before)->duration, int64_t(10)); // up to the next
        v.f.project.timeline()->setPlayhead(20); // inside the first caption
        const quint64 other = v.c.addSubtitleAtPlayhead();
        QCOMPARE(ops::findClip(v.f.view(), other)->track.index, 1);
        // Adjustment clips and generators come from the effects library like titles.
        v.f.project.timeline()->setPlayhead(200);
        QVERIFY(v.c.makeGeneratorClip(QStringLiteral("adjustment_clip"), {25, 1}));
        QCOMPARE(v.c.makeGeneratorClip(QStringLiteral("adjustment_clip"), {25, 1})->kind,
            ClipKind::Adjustment);
        const auto noise = v.c.makeGeneratorClip(QStringLiteral("noise"), {25, 1});
        QVERIFY(noise && noise->kind == ClipKind::Generator);
        QCOMPARE(noise->generator, QStringLiteral("noise"));
        QVERIFY(noise->title.values.contains(QStringLiteral("grain")));
    }
};

QTEST_MAIN(TestTimelineDrop)
#include "test_timelinedrop.moc"
