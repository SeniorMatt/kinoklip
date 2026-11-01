// SPDX-License-Identifier: GPL-3.0-or-later
// JSON round-trip tests: value types, full project save/load, error handling.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Serialization.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

/// A timeline exercising every field the serializer knows about.
TimelineData richTimeline()
{
    TimelineData d = timeline();
    d.name = QStringLiteral("Main ✂ Timeline");
    d.fps = Rational {30000, 1001};
    d.width = 3840;
    d.height = 2160;
    d.markIn = 12;
    d.markOut = 99;
    d.markers.append(Marker {5, QStringLiteral("Start"), QColor(255, 0, 0)});

    ClipData media = mediaClip(0, 50, 3, 500, QStringLiteral("Interview"));
    media.label = QColor(10, 200, 30);
    media.speed = 1.5;
    media.enabled = false;
    media.transform.animated(keys::PosX, 0).setKey(0, -12.5, Interp::EaseInOut);
    media.transform.animated(keys::PosX, 0).setKey(30, 0.1 + 0.2, Interp::Hold);
    media.transform.setBase(keys::Opacity, 75.0);
    media.transform.setStatic(keys::FlipH, true);
    media.audio.setBase(keys::VolumeDb, -6.0);
    EffectInstance blur
        = EffectRegistry::instance().createEffect(QStringLiteral("gaussian_blur"), d.allocateId());
    blur.params.animated(QStringLiteral("radius"), 5).setKey(10, 20.0);
    media.effects.append(blur);
    media.effects.append(
        EffectRegistry::instance().createEffect(QStringLiteral("drop_shadow"), d.allocateId()));
    media.transitionIn = EffectRegistry::instance().createTransition(QStringLiteral("wipe"), 12);
    media.transitionOut = TransitionData {};
    const quint64 v = ops::overwriteClip(d, kV1, media);
    const quint64 a = ops::overwriteClip(d, kA1, mediaClip(0, 50));
    ops::linkClips(d, {v, a});

    ClipData title;
    title.kind = ClipKind::Title;
    title.name = QStringLiteral("Title");
    title.duration = 25;
    title.start = 60;
    title.title = EffectRegistry::instance().defaultParams(QStringLiteral("text_title"));
    title.title.setStatic(keys::Text, QStringLiteral("Hello\n\"World\""));
    title.title.setStatic(keys::Color, QColor(1, 2, 3, 4));
    ops::overwriteClip(d, kV2, title);

    ClipData solid;
    solid.kind = ClipKind::Solid;
    solid.duration = 10;
    solid.start = 100;
    solid.solidColor = QColor(0x12, 0x34, 0x56);
    ops::overwriteClip(d, kV1, solid);
    d.audioTracks[1].muted = true;
    d.videoTracks[1].locked = true;
    d.videoTracks[1].name = QStringLiteral("Titles");
    d.videoTracks[1].height = 80;
    return d;
}

} // namespace

class TestSerialization : public QObject
{
    Q_OBJECT
private slots:
    void timelineRoundTrip()
    {
        const TimelineData original = richTimeline();
        KK_VERIFY_INVARIANTS(original);
        const QJsonObject json = serialization::timelineToJson(original);
        // Go through text to catch number formatting issues too.
        const QJsonDocument doc = QJsonDocument::fromJson(QJsonDocument(json).toJson());
        TimelineData restored;
        QString error;
        QVERIFY2(
            serialization::timelineFromJson(doc.object(), restored, &error), qPrintable(error));
        QCOMPARE(restored, original);
    }

    void clipRoundTrip()
    {
        const TimelineData d = richTimeline();
        for (const ClipData& c : d.videoTracks[0].clips)
            QCOMPARE(serialization::clipFromJson(serialization::clipToJson(c)), c);
    }

    void variantRoundTrip()
    {
        const QVariantList values {true, 42, 2.5, QStringLiteral("text"), QColor(1, 2, 3, 4)};
        for (const QVariant& v : values)
            QCOMPARE(serialization::variantFromJson(serialization::variantToJson(v)), v);
    }

    void invalidTimelineIsRejected()
    {
        TimelineData d = richTimeline();
        QJsonObject json = serialization::timelineToJson(d);
        // Make two clips on V1 overlap.
        QJsonArray video = json[QLatin1String("videoTracks")].toArray();
        QJsonObject v1 = video[0].toObject();
        QJsonArray clips = v1[QLatin1String("clips")].toArray();
        QJsonObject second = clips[1].toObject();
        second[QLatin1String("start")] = 10;
        clips[1] = second;
        v1[QLatin1String("clips")] = clips;
        video[0] = v1;
        json[QLatin1String("videoTracks")] = video;
        TimelineData out;
        QString error;
        QVERIFY(!serialization::timelineFromJson(json, out, &error));
        QVERIFY(error.contains(QLatin1String("overlap")));
    }

    void projectSaveLoad()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("test.kkproj"));

        Project p;
        p.setName(QStringLiteral("My Film"));
        p.beginEdit(QStringLiteral("Setup"));
        p.timeline()->mutableData() = richTimeline();
        const QString binId = p.mediaPool()->addBin(QStringLiteral("Footage"));
        MediaItem item;
        item.path = dir.filePath(QStringLiteral("clip.mp4"));
        item.binId = binId;
        item.info.valid = true;
        item.info.path = item.path;
        item.info.hasVideo = true;
        item.info.width = 1920;
        item.info.height = 1080;
        item.info.frameRate = Rational {25, 1};
        item.info.durationUs = 20'000'000;
        item.info.videoCodec = QStringLiteral("h264");
        p.mediaPool()->addItem(item);
        p.endEdit();
        QVERIFY(p.isDirty());

        QString error;
        QVERIFY2(p.save(path, &error), qPrintable(error));
        QVERIFY(!p.isDirty());
        QCOMPARE(p.filePath(), path);

        Project loaded;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        QCOMPARE(loaded.name(), QStringLiteral("My Film"));
        QCOMPARE(loaded.timeline()->data(), p.timeline()->data());
        QCOMPARE(loaded.mediaPool()->state(), p.mediaPool()->state());
        QCOMPARE(loaded.filePath(), path);
        QVERIFY(!loaded.isDirty());
        QCOMPARE(loaded.undoStack()->count(), 0);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root.value(QLatin1String("formatVersion")).toInt(), serialization::kFormatVersion);
    }

    void saveCopyKeepsDocumentState()
    {
        QTemporaryDir dir;
        Project p;
        p.setName(QStringLiteral("x"));
        QVERIFY(p.saveCopy(dir.filePath(QStringLiteral("auto.kkproj"))));
        QVERIFY(p.isDirty());
        QVERIFY(p.filePath().isEmpty());
    }

    void mediaRelocatedWithProjectFolder()
    {
        QTemporaryDir dir;
        const QDir oldDir(dir.filePath(QStringLiteral("old")));
        const QDir newDir(dir.filePath(QStringLiteral("new")));
        QVERIFY(QDir().mkpath(newDir.filePath(QStringLiteral("media"))));
        QFile media(newDir.filePath(QStringLiteral("media/a.mov")));
        QVERIFY(media.open(QIODevice::WriteOnly));
        media.close();

        MediaPoolState state;
        MediaItem item;
        item.id = QStringLiteral("m1");
        item.path = oldDir.filePath(QStringLiteral("media/a.mov")); // does not exist any more
        state.items.append(item);
        const QJsonObject json = serialization::mediaPoolToJson(state, oldDir);
        const MediaPoolState loaded = serialization::mediaPoolFromJson(json, newDir);
        QCOMPARE(loaded.items[0].path, newDir.filePath(QStringLiteral("media/a.mov")));
    }

    void loadErrors()
    {
        QTemporaryDir dir;
        Project p;
        QString error;
        QVERIFY(!p.load(dir.filePath(QStringLiteral("missing.kkproj")), &error));
        QVERIFY(!error.isEmpty());

        auto writeFile = [&](const QString& name, const QByteArray& content) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
        };
        writeFile(QStringLiteral("garbage.kkproj"), "not json");
        QVERIFY(!p.load(dir.filePath(QStringLiteral("garbage.kkproj")), &error));
        writeFile(QStringLiteral("other.kkproj"), R"({"hello": 1})");
        QVERIFY(!p.load(dir.filePath(QStringLiteral("other.kkproj")), &error));
        writeFile(QStringLiteral("future.kkproj"), R"({"formatVersion": 999})");
        QVERIFY(!p.load(dir.filePath(QStringLiteral("future.kkproj")), &error));
        QVERIFY(error.contains(QLatin1String("newer")));
        // The current project is untouched after failures.
        QCOMPARE(p.timeline()->data().videoTracks.size(), size_t(2));
    }
};

QTEST_GUILESS_MAIN(TestSerialization)
#include "test_serialization.moc"
