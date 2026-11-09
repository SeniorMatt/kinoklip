// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for File > Import > Timeline (spec 51.4, 51.5): a .otio file becomes a timeline of the
// project in one undo step; media that exists is imported, media that is missing is offline;
// File > Export > OpenTimelineIO writes a file the importer reads back.
#include "app/TimelineImport.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "export/Otio.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <QUrl>

using namespace kk;
namespace ops = editops;

namespace {

QString time(double value, double rate)
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"RationalTime.1","rate":%2,"value":%1})")
        .arg(value)
        .arg(rate);
}

QString range(double start, double duration, double rate)
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"TimeRange.1","start_time":%1,"duration":%2})")
        .arg(time(start, rate), time(duration, rate));
}

QString clip(const QString& name, const QString& url, double start, double duration)
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"Clip.2","metadata":{},"name":"%1","source_range":%2,
        "effects":[{"OTIO_SCHEMA":"Effect.1","name":"x","effect_name":"X"}],"markers":[],
        "enabled":true,"active_media_reference_key":"DEFAULT_MEDIA",
        "media_references":{"DEFAULT_MEDIA":{"OTIO_SCHEMA":"ExternalReference.1","name":"",
        "available_range":%2,"target_url":"%3"}}})")
        .arg(name, range(start, duration, 25), url);
}

} // namespace

class TestTimelineImport : public QObject
{
    Q_OBJECT
private slots:
    void importsExistingAndMissingMediaInOneUndoStep()
    {
        const QString real = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4");
        QVERIFY(QFile::exists(real));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("cut.otio"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QStringLiteral(R"({"OTIO_SCHEMA":"Timeline.1","metadata":{},"name":"Imported cut",
            "global_start_time":%1,"tracks":{"OTIO_SCHEMA":"Stack.1","children":[
            {"OTIO_SCHEMA":"Track.1","kind":"Video","children":[%2,%3]}]}})")
                    .arg(time(90000, 25),
                        clip(QStringLiteral("Real"), QUrl::fromLocalFile(real).toString(), 25, 50),
                        clip(QStringLiteral("Gone"), QStringLiteral("clips/gone.mov"), 0, 25))
                    .toUtf8());
        f.close();

        Project p;
        const int timelines = int(p.timelineUids().size());
        const int items = p.mediaPool()->items().size();
        const TimelineImportReport r = importOtioFile(p, file);
        QVERIFY2(r.uid != 0, qPrintable(r.error));
        QCOMPARE(r.clipCount, 2);
        QCOMPARE(r.mediaAdded, 1);
        QCOMPARE(r.offline, QStringList {dir.filePath(QStringLiteral("clips/gone.mov"))});
        QVERIFY(r.skipped.join(QLatin1Char(' ')).contains(QStringLiteral("Effect.1")));

        QCOMPARE(int(p.timelineUids().size()), timelines + 1);
        const auto data = p.timelineData(r.uid);
        QVERIFY(data);
        QCOMPARE(data->name, QStringLiteral("Imported cut"));
        QCOMPARE(data->fps, (Rational {25, 1})); // the project runs at 24: the timeline keeps 25
        QVERIFY(!data->useProjectSettings);
        QCOMPARE(data->startTimecode, int64_t(90000));
        const auto& clips = data->videoTracks[0].clips;
        QCOMPARE(clips.size(), size_t(2));
        const MediaItem* realItem = p.mediaPool()->itemByPath(real);
        QVERIFY(realItem);
        QCOMPARE(clips[0].mediaId, realItem->id);
        QVERIFY(realItem->info.valid);
        QCOMPARE(clips[0].sourceDurationUs, realItem->info.durationUs);
        const MediaItem* gone = p.mediaPool()->item(clips[1].mediaId);
        QVERIFY(gone);
        p.mediaPool()->recheckOffline();
        QVERIFY(p.mediaPool()->isOffline(gone->id));
        QVERIFY(!p.mediaPool()->isOffline(realItem->id));

        // One Undo takes the timeline and both media items away; Redo brings them back.
        p.undo();
        QCOMPARE(int(p.timelineUids().size()), timelines);
        QCOMPARE(p.mediaPool()->items().size(), items);
        p.redo();
        QCOMPARE(int(p.timelineUids().size()), timelines + 1);
        QCOMPARE(p.mediaPool()->items().size(), items + 3); // 2 files and the timeline's item
    }

    void aSecondImportWithTheSameNameGetsANumber()
    {
        Project p;
        TimelineData d
            = makeDefaultTimeline(QStringLiteral("Timeline 1"), {24, 1}, 1920, 1080, 1, 1);
        const quint64 uid = p.importTimeline(d);
        QCOMPARE(p.timelineData(uid)->name, QStringLiteral("Timeline 1 1"));
        QVERIFY(p.timelineData(uid)->useProjectSettings);
        QVERIFY(uid != p.activeTimelineUid());
    }

    void exportedTimelineImportsBack()
    {
        Project p;
        const QString real = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4");
        const ImportResult imported = p.importMedia({real});
        QCOMPARE(imported.importedIds.size(), 1);
        const MediaItem* item = p.mediaPool()->item(imported.importedIds[0]);
        ClipData c;
        c.kind = ClipKind::Media;
        c.mediaId = item->id;
        c.name = QStringLiteral("clip");
        c.start = 12;
        c.duration = 30;
        c.sourceInUs = 500000;
        c.sourceDurationUs = item->info.durationUs;
        p.beginEdit(QStringLiteral("test"));
        ops::overwriteClip(p.timeline()->mutableData(), {TrackType::Video, 0}, c);
        p.endEdit();
        const TimelineData& before = p.timeline()->data();
        const QByteArray text = otio::timelineToOtio(before, p.mediaPool());

        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("out.otio"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(text);
        f.close();
        const TimelineImportReport r = importOtioFile(p, file);
        QVERIFY2(r.uid != 0, qPrintable(r.error));
        QVERIFY(r.offline.isEmpty());
        QCOMPARE(r.mediaAdded, 0); // the file is already in the pool: reused
        const auto data = p.timelineData(r.uid);
        const ClipData& back = data->videoTracks[0].clips[0];
        QCOMPARE(back.mediaId, c.mediaId);
        QCOMPARE(back.start, c.start);
        QCOMPARE(back.duration, c.duration);
        QCOMPARE(back.sourceInUs, c.sourceInUs);
        QCOMPARE(data->startTimecode, before.startTimecode);
    }

    void aFileThatIsNotOtioChangesNothing()
    {
        Project p;
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("bad.otio"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"OTIO_SCHEMA\":\"Gap.1\"}");
        f.close();
        const int items = p.mediaPool()->items().size();
        const int undo = p.undoStack()->count();
        const TimelineImportReport r = importOtioFile(p, file);
        QCOMPARE(r.uid, quint64(0));
        QVERIFY(!r.error.isEmpty());
        QCOMPARE(p.mediaPool()->items().size(), items);
        QCOMPARE(p.undoStack()->count(), undo);
        QVERIFY(!importOtioFile(p, dir.filePath(QStringLiteral("missing.otio"))).error.isEmpty());
    }

    void theActionsExist()
    {
        initResources();
        ActionRegistry registry;
        QVERIFY(registry.loadDefaultKeymap());
        registerStandardActions(registry);
        QVERIFY(registry.contains(QStringLiteral("file.import_timeline")));
        QVERIFY(registry.contains(QStringLiteral("file.export_otio")));
    }
};

QTEST_MAIN(TestTimelineImport)
#include "test_timelineimport.moc"
