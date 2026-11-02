// SPDX-License-Identifier: GPL-3.0-or-later
// Robustness against bad input: damaged project files (a deterministic JSON fuzzer over a real
// project with media, a title, a transition, effects and keyframes), offline and moved media,
// bad media files, and large projects (2,000 clips load/save time). Every project that loads
// is rendered and mixed at a few positions, so a crash anywhere downstream shows up here.
// Run under ASan/UBSan for the full value (see docs/releases notes / readme).
#include "../export/ExportTestUtil.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "export/RenderCommand.h"
#include "render/AudioMixer.h"
#include "render/Compositor.h"
#include "render/RenderTypes.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <vector>

using namespace kk;

namespace {

/// Replacement values the fuzzer puts into the JSON tree.
QJsonValue hostileValue(QRandomGenerator& rng)
{
    switch (rng.bounded(16)) {
    case 0:
        return -1;
    case 1:
        return 0;
    case 2:
        return 9.0e18; // beyond int64 when converted
    case 3:
        return 1.0e300;
    case 4:
        return -1.0e300;
    case 5:
        return QString();
    case 6:
        return QStringLiteral("garbage");
    case 7:
        return QJsonValue::Null;
    case 8:
        return QJsonArray {1, 2, 3};
    case 9:
        return QJsonObject {{QStringLiteral("x"), 1}};
    case 10:
        return true;
    case 11:
        return 2147483647;
    case 12:
        return -2147483647;
    case 13:
        return 1.0e-300;
    case 14:
        return 100000;
    default:
        return int(rng.bounded(200)) - 50;
    }
}

/// Replaces (or deletes) one random leaf/subtree of @p value. Returns the mutated value.
QJsonValue mutate(const QJsonValue& value, QRandomGenerator& rng, int depth = 0)
{
    // Stop descending with some probability, or at leaves.
    const bool container = value.isObject() || value.isArray();
    if (!container || (depth > 0 && rng.bounded(5) == 0))
        return hostileValue(rng);
    if (value.isObject()) {
        QJsonObject o = value.toObject();
        if (o.isEmpty())
            return hostileValue(rng);
        const QStringList keys = o.keys();
        const QString key = keys.at(int(rng.bounded(qsizetype(keys.size()))));
        if (rng.bounded(8) == 0)
            o.remove(key);
        else
            o[key] = mutate(o.value(key), rng, depth + 1);
        return o;
    }
    QJsonArray a = value.toArray();
    if (a.isEmpty())
        return hostileValue(rng);
    const int i = int(rng.bounded(qsizetype(a.size())));
    switch (rng.bounded(6)) {
    case 0:
        a.removeAt(i);
        break;
    case 1:
        a.append(a.at(i)); // duplicate (same ids)
        break;
    default:
        a[i] = mutate(a.at(i), rng, depth + 1);
        break;
    }
    return a;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

/// Renders and mixes @p project at a few frames (start, middle, end, past the end).
void exercise(const Project& project, Compositor& compositor, AudioMixer& mixer)
{
    auto timeline = std::make_shared<const TimelineData>(project.timeline()->data());
    const auto media = makeMediaPathMap(project.mediaPool()->state());
    compositor.setMediaPaths(media);
    mixer.setMediaPaths(media);
    int64_t end = 0;
    for (TrackType type : {TrackType::Video, TrackType::Audio})
        for (const TrackData& t : timeline->tracks(type))
            if (!t.clips.empty())
                end = std::max(end, t.clips.back().end());
    std::vector<float> audio(2 * 1024);
    for (int64_t frame : {int64_t(0), end / 2, std::max<int64_t>(0, end - 1), end + 5}) {
        (void)compositor.renderImage(timeline, frame, QSize(160, 90), Quality::Quarter);
        mixer.mix(*timeline, framesToSamples48k(frame, timeline->fps), 1024, audio.data());
    }
}

} // namespace

class TestRobustness : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
    }

    /// Damaged project files never crash the loader, and whatever loads renders and mixes.
    void fuzzProjectFiles()
    {
        Project project;
        QString why;
        if (!test::buildExportProject(project, &why))
            QSKIP(qPrintable(why));
        // Effects with keyframes and a transition out, so those code paths get damaged too.
        const EffectRegistry& registry = EffectRegistry::instance();
        project.beginEdit(QStringLiteral("fx"));
        TimelineData& tl = project.timeline()->mutableData();
        ClipData& first = tl.videoTracks[0].clips[0];
        int added = 0;
        for (const EffectDescriptor& d : registry.all()) {
            if (d.category != EffectCategory::VideoEffect || added >= 4)
                continue;
            first.effects.append(registry.createEffect(d.id, tl.nextId++));
            ++added;
        }
        first.transform.values[keys::Opacity].setKey(0, 0.2);
        first.transform.values[keys::Opacity].setKey(20, 1.0);
        tl.videoTracks[1].clips[0].transitionOut
            = registry.createTransition(QStringLiteral("cross_dissolve"), 8);
        project.endEdit();

        QTemporaryDir dir;
        const QString base = dir.filePath(QStringLiteral("base.kkproj"));
        QVERIFY(project.save(base));
        QFile f(base);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QVERIFY(!root.isEmpty());

        Compositor compositor(Compositor::Options {true}); // CPU: deterministic, any QPA
        AudioMixer mixer;
        const int iterations = qEnvironmentVariableIntValue("KK_FUZZ_ITERATIONS") > 0
            ? qEnvironmentVariableIntValue("KK_FUZZ_ITERATIONS")
            : 250;
        QRandomGenerator rng(0x6b696e6f); // fixed seed: reproducible failures
        int loaded = 0;
        const QString path = dir.filePath(QStringLiteral("fuzz.kkproj"));
        for (int i = 0; i < iterations; ++i) {
            QJsonValue v = root;
            const int mutations = 1 + int(rng.bounded(3));
            for (int m = 0; m < mutations; ++m)
                v = mutate(v, rng);
            QByteArray bytes = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
            if (i % 25 == 0)
                bytes.truncate(int(rng.bounded(qsizetype(bytes.size())))); // truncated file
            QVERIFY(writeFile(path, bytes));
            Project p;
            QString error;
            if (!p.load(path, &error)) {
                QVERIFY2(!error.isEmpty(), "a failed load must say why");
                continue;
            }
            ++loaded;
            QString invariant;
            QVERIFY2(editops::checkInvariants(p.timeline()->data(), &invariant),
                qPrintable(QStringLiteral("iteration %1: %2").arg(i).arg(invariant)));
            exercise(p, compositor, mixer);
        }
        qInfo("fuzz: %d of %d damaged projects loaded and rendered", loaded, iterations);
        QVERIFY(loaded > 0);
    }

    /// Hand-written damaged files: each must fail cleanly or load sanely.
    void hostileProjectFiles_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<bool>("loads");
        QTest::newRow("empty") << QByteArray() << false;
        QTest::newRow("binary") << QByteArray("\x00\xff\x13\x37", 4) << false;
        QTest::newRow("array") << QByteArray("[1,2,3]") << false;
        QTest::newRow("no version") << QByteArray(R"({"timeline":{}})") << false;
        QTest::newRow("newer version") << QByteArray(R"({"formatVersion":99})") << false;
        QTest::newRow("version only") << QByteArray(R"({"formatVersion":1})") << true;
        QTest::newRow("zero fps") << QByteArray(
            R"({"formatVersion":1,"timeline":{"fps":{"num":0,"den":0}}})")
                                  << false;
        QTest::newRow("absurd fps")
            << QByteArray(R"({"formatVersion":1,"timeline":{"fps":{"num":2000000000,"den":1}}})")
            << false;
        QTest::newRow("huge resolution")
            << QByteArray(R"({"formatVersion":1,"timeline":{"width":100000,"height":100000}})")
            << false;
        QTest::newRow("negative size")
            << QByteArray(R"({"formatVersion":1,"timeline":{"width":-5,"height":1080}})") << false;
        QTest::newRow("clip far away")
            << QByteArray(R"({"formatVersion":1,"timeline":{"nextId":10,"videoTracks":[{"id":1,
                "clips":[{"id":2,"kind":"solid","start":9000000000000000000,"duration":5}]}]}})")
            << false;
        QTest::newRow("absurd speed")
            << QByteArray(R"({"formatVersion":1,"timeline":{"nextId":10,"videoTracks":[{"id":1,
                "clips":[{"id":2,"kind":"solid","start":0,"duration":5,"speed":1e300}]}]}})")
            << false;
        QTest::newRow("transition longer than clip")
            << QByteArray(R"({"formatVersion":1,"timeline":{"nextId":10,"videoTracks":[{"id":1,
                "clips":[{"id":2,"kind":"solid","start":0,"duration":5,
                "transitionIn":{"type":"cross_dissolve","duration":500}},
                {"id":3,"kind":"solid","start":5,"duration":5,
                "transitionIn":{"type":"no_such_transition","duration":-7}}]}]}})")
            << true;
        QTest::newRow("unknown effect and media")
            << QByteArray(R"({"formatVersion":1,"timeline":{"nextId":10,"videoTracks":[{"id":1,
                "clips":[{"id":2,"kind":"media","mediaId":"nope","start":0,"duration":5,
                "effects":[{"id":3,"type":"no.such.effect","params":{"values":{"x":
                {"base":1e300,"keys":[{"frame":-5,"value":1e300,"interp":"zzz"}]}}}}]}]}]}})")
            << true;
    }

    void hostileProjectFiles()
    {
        QFETCH(QByteArray, json);
        QFETCH(bool, loads);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.kkproj"));
        QVERIFY(writeFile(path, json));
        Project p;
        QString error;
        const bool ok = p.load(path, &error);
        QCOMPARE(ok, loads);
        if (!ok) {
            QVERIFY(!error.isEmpty());
            return;
        }
        Compositor compositor(Compositor::Options {true});
        AudioMixer mixer;
        exercise(p, compositor, mixer);
    }

    /// Media moved: the project still opens with the clips offline (they render as nothing,
    /// export does not crash); media found next to the project by file name is relinked.
    void offlineAndMovedMedia()
    {
        QTemporaryDir dir;
        const QString mediaDir = dir.filePath(QStringLiteral("media"));
        QVERIFY(QDir().mkpath(mediaDir));
        const QString media = mediaDir + QStringLiteral("/clip.mp4");
        QVERIFY(QFile::copy(test::fixture("h264_25fps.mp4"), media));
        Project project;
        const ImportResult r = project.importMedia({media});
        QCOMPARE(r.importedIds.size(), 1);
        project.beginEdit(QStringLiteral("clip"));
        TimelineData& tl = project.timeline()->mutableData();
        test::addClip(tl, TrackType::Video, 0,
            test::mediaClip(tl, r.importedIds[0], 0, 50, 0, framesToUs(50, tl.fps)));
        test::addClip(tl, TrackType::Audio, 0,
            test::mediaClip(tl, r.importedIds[0], 0, 50, 0, framesToUs(50, tl.fps)));
        project.endEdit();
        const QString projectPath = dir.filePath(QStringLiteral("p/p.kkproj"));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("p"))));
        QVERIFY(project.save(projectPath));

        // 1. Offline: the file is gone.
        QVERIFY(QFile::rename(media, dir.filePath(QStringLiteral("elsewhere.mp4"))));
        {
            Project p;
            QVERIFY(p.load(projectPath));
            QVERIFY(p.mediaPool()->item(r.importedIds[0]));
            QVERIFY(!QFileInfo::exists(p.mediaPool()->item(r.importedIds[0])->path));
            // Polish round 2: the pool knows it is offline (timeline and viewers mark it).
            QVERIFY(p.mediaPool()->isOffline(r.importedIds[0]));
            QCOMPARE(p.mediaPool()->offlineCount(), 1);
            Compositor compositor(Compositor::Options {true});
            AudioMixer mixer;
            exercise(p, compositor, mixer);
        }
        // 2. Moved next to the project (same file name): relinked on load.
        QVERIFY(QFile::rename(dir.filePath(QStringLiteral("elsewhere.mp4")),
            dir.filePath(QStringLiteral("p/clip.mp4"))));
        {
            Project p;
            QVERIFY(p.load(projectPath));
            QCOMPARE(QFileInfo(p.mediaPool()->item(r.importedIds[0])->path).canonicalFilePath(),
                QFileInfo(dir.filePath(QStringLiteral("p/clip.mp4"))).canonicalFilePath());
        }
        // 3. Moved into a folder chosen by the user: relinkFromFolder() finds it by name.
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("new"))));
        QVERIFY(QFile::rename(dir.filePath(QStringLiteral("p/clip.mp4")),
            dir.filePath(QStringLiteral("new/clip.mp4"))));
        {
            Project p;
            QVERIFY(p.load(projectPath));
            p.beginEdit(QStringLiteral("Relink"));
            const int relinked
                = p.mediaPool()->relinkFromFolder(dir.filePath(QStringLiteral("new")));
            p.endEdit();
            QCOMPARE(relinked, 1);
            QVERIFY(QFileInfo::exists(p.mediaPool()->item(r.importedIds[0])->path));
            QVERIFY(p.isDirty());
            QVERIFY(!p.mediaPool()->isOffline(r.importedIds[0]));
        }
        // 4. Polish round 2: Relink searches subfolders (the parent folder is picked), and
        // "Relink Selected Clips" relinks only the given items.
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("deep/a/b"))));
        QVERIFY(QFile::rename(dir.filePath(QStringLiteral("new/clip.mp4")),
            dir.filePath(QStringLiteral("deep/a/b/clip.mp4"))));
        {
            Project p;
            QVERIFY(p.load(projectPath));
            QVERIFY(p.mediaPool()->isOffline(r.importedIds[0]));
            QSignalSpy offline(p.mediaPool(), &MediaPool::offlineChanged);
            QCOMPARE(p.mediaPool()->relinkFromFolder(
                         dir.filePath(QStringLiteral("deep")), {QStringLiteral("other")}),
                0);
            QCOMPARE(p.mediaPool()->relinkFromFolder(
                         dir.filePath(QStringLiteral("deep")), {r.importedIds[0]}),
                1);
            QVERIFY(!p.mediaPool()->isOffline(r.importedIds[0]));
            QCOMPARE(offline.count(), 1);
        }
    }

    /// Bad media files are refused with a reason; a truncated file that probes as valid
    /// decodes until its data ends without crashing.
    void badMediaFiles()
    {
        QTemporaryDir dir;
        auto make = [&](const char* name, const QByteArray& data) {
            const QString p = dir.filePath(QLatin1String(name));
            writeFile(p, data);
            return p;
        };
        QFile src(test::fixture("h264_25fps.mp4"));
        QVERIFY(src.open(QIODevice::ReadOnly));
        const QByteArray mp4 = src.readAll();
        QByteArray noise(200000, '\0');
        QRandomGenerator rng(7);
        for (char& c : noise)
            c = char(rng.bounded(256));
        const QStringList bad = {make("zero.mp4", {}), make("noise.mov", noise),
            make("text.mkv", "this is not a video\n"), make("head.mp4", mp4.left(4000)), dir.path(),
            dir.filePath(QStringLiteral("does-not-exist.mp4"))};
        Project project;
        const ImportResult r = project.importMedia(bad);
        QCOMPARE(r.importedIds.size(), 0);
        QCOMPARE(r.warnings.size(), bad.size());
        // Damaged in the middle: random bytes over a quarter of the file.
        QByteArray damaged = mp4;
        for (int i = damaged.size() / 2; i < damaged.size() * 3 / 4; ++i)
            damaged[i] = char(rng.bounded(256));
        const QString damagedPath = make("damaged.mp4", damaged);
        const ImportResult d = project.importMedia({damagedPath});
        if (!d.importedIds.isEmpty()) {
            project.beginEdit(QStringLiteral("clip"));
            TimelineData& tl = project.timeline()->mutableData();
            const auto& info = project.mediaPool()->state().items.back().info;
            const int64_t frames = std::max<int64_t>(1, usToFrames(info.durationUs, tl.fps));
            test::addClip(tl, TrackType::Video, 0,
                test::mediaClip(tl, d.importedIds[0], 0, frames, 0, info.durationUs));
            test::addClip(tl, TrackType::Audio, 0,
                test::mediaClip(tl, d.importedIds[0], 0, frames, 0, info.durationUs));
            project.endEdit();
            Compositor compositor(Compositor::Options {true});
            AudioMixer mixer;
            auto timeline = std::make_shared<const TimelineData>(tl);
            compositor.setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
            mixer.setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
            std::vector<float> audio(2 * 1920);
            for (int64_t f = 0; f < frames; f += 3) {
                (void)compositor.renderImage(timeline, f, QSize(160, 90), Quality::Quarter);
                mixer.mix(*timeline, framesToSamples48k(f, tl.fps), 1920, audio.data());
            }
        }
    }

    /// Odd timeline rates with sources of other rates, sizes, rotation and alpha, edited
    /// (trims, speed change, dissolve) and exported through the --render code path.
    void oddFrameRatesEditAndExport_data()
    {
        QTest::addColumn<int>("num");
        QTest::addColumn<int>("den");
        QTest::newRow("23.976") << 24000 << 1001;
        QTest::newRow("59.94") << 60000 << 1001;
        QTest::newRow("29.97") << 30000 << 1001;
    }

    void oddFrameRatesEditAndExport()
    {
        QFETCH(int, num);
        QFETCH(int, den);
        Project project;
        const QStringList sources
            = {test::fixture("h264_2997.mp4"), test::fixture("h264_60fps.mp4"),
                test::fixture("rotated_portrait.mp4"), test::fixture("vfr_odd.mp4"),
                test::fixture("prores4444_alpha.mov"), test::fixture("still_alpha.png")};
        const ImportResult r = project.importMedia(sources);
        QCOMPARE(r.importedIds.size(), sources.size());
        const EffectRegistry& registry = EffectRegistry::instance();
        project.beginEdit(QStringLiteral("edit"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.fps = {num, den};
        tl.width = 640;
        tl.height = 360;
        // V1: four sources back to back, 1 s each, cut 0.3 s into the source, a dissolve.
        const int64_t second = usToFrames(1'000'000, tl.fps);
        for (int i = 0; i < 4; ++i) {
            ClipData c = test::mediaClip(
                tl, r.importedIds[i], i * second, second, 300'000, framesToUs(second, tl.fps));
            if (i == 1) {
                c.speed = 1.5; // speed change on a 60 fps source
                c.sourceDurationUs = int64_t(framesToUs(second, tl.fps) * 1.5);
            }
            if (i == 2)
                c.transitionIn = registry.createTransition(QStringLiteral("cross_dissolve"), 6);
            test::addClip(tl, TrackType::Video, 0, c);
        }
        // V2: alpha ProRes, then the alpha still over it.
        test::addClip(tl, TrackType::Video, 1,
            test::mediaClip(tl, r.importedIds[4], 0, second, 0, framesToUs(second, tl.fps)));
        test::addClip(tl, TrackType::Video, 1,
            test::mediaClip(tl, r.importedIds[5], second, second, 0, framesToUs(second, tl.fps)));
        // A1: audio of the 29.97 clip across the whole timeline.
        test::addClip(tl, TrackType::Audio, 0,
            test::mediaClip(
                tl, r.importedIds[0], 0, 4 * second, 0, framesToUs(4 * second, tl.fps)));
        project.endEdit();
        QString invariant;
        QVERIFY2(editops::checkInvariants(project.timeline()->data(), &invariant),
            qPrintable(invariant));

        QTemporaryDir dir;
        const QString projectPath = dir.filePath(QStringLiteral("odd.kkproj"));
        QVERIFY(project.save(projectPath));
        const QString out = dir.filePath(QStringLiteral("odd.mp4"));
        QCOMPARE(runRender(projectPath, out), 0);
        const media::MediaInfo info = media::probe(out);
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.frameRate.num, num);
        QCOMPARE(info.frameRate.den, den);
        QCOMPARE(info.width, 640);
        QCOMPARE(info.height, 360);
        QVERIFY(info.hasAudio);
        const int64_t expectedUs = framesToUs(4 * second, tl.fps);
        QVERIFY2(std::abs(info.durationUs - expectedUs) <= framesToUs(2, tl.fps),
            qPrintable(QStringLiteral("duration %1 us, expected %2")
                           .arg(info.durationUs)
                           .arg(expectedUs)));
        // Every frame decodes (no holes at clip boundaries / the speed-changed clip).
        media::VideoDecoder decoder;
        QVERIFY(decoder.open(out));
        for (int64_t f = 0; f < 4 * second; f += second / 4)
            QVERIFY2(
                decoder.decodeFrame(framesToUs(f, tl.fps) + 1), qPrintable(QString::number(f)));
    }

    /// A very long clip (opt-in: KK_LONG_MEDIA=<file of an hour or more>, e.g. the 2 h file
    /// made by `ffmpeg -f lavfi -i color=s=320x180:r=25:d=7200 ...`): import, put the whole
    /// clip on the timeline, render and mix near the end, and time the far seeks.
    void veryLongClip()
    {
        const QString path = qEnvironmentVariable("KK_LONG_MEDIA");
        if (path.isEmpty())
            QSKIP("set KK_LONG_MEDIA to a long media file");
        Project project;
        const ImportResult r = project.importMedia({path});
        QCOMPARE(r.importedIds.size(), 1);
        const media::MediaInfo& info = project.mediaPool()->item(r.importedIds[0])->info;
        QVERIFY(info.durationUs > 3600LL * 1'000'000);
        project.beginEdit(QStringLiteral("long"));
        TimelineData& tl = project.timeline()->mutableData();
        const int64_t frames = usToFrames(info.durationUs, tl.fps);
        test::addClip(tl, TrackType::Video, 0,
            test::mediaClip(tl, r.importedIds[0], 0, frames, 0, info.durationUs));
        test::addClip(tl, TrackType::Audio, 0,
            test::mediaClip(tl, r.importedIds[0], 0, frames, 0, info.durationUs));
        project.endEdit();
        Compositor compositor(Compositor::Options {true});
        AudioMixer mixer;
        auto timeline = std::make_shared<const TimelineData>(tl);
        compositor.setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
        mixer.setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
        std::vector<float> audio(2 * 1920);
        for (int64_t f : {int64_t(0), frames / 2, frames - 2, frames - 1, int64_t(10)}) {
            QElapsedTimer t;
            t.start();
            const QImage image = compositor.renderImage(timeline, f, QSize(160, 90));
            const qint64 videoMs = t.restart();
            mixer.mix(*timeline, framesToSamples48k(f, tl.fps), 1920, audio.data());
            qInfo("frame %lld of %lld: video %lld ms, audio %lld ms", qlonglong(f),
                qlonglong(frames), videoMs, t.elapsed());
            QVERIFY(!image.isNull());
        }
    }

    /// 2,000 clips with parameters: save and load stay well under a second.
    void largeProjectLoadSave()
    {
        Project project;
        project.beginEdit(QStringLiteral("big"));
        TimelineData& tl = project.timeline()->mutableData();
        constexpr int kClips = 2000;
        for (int i = 0; i < kClips; ++i) {
            ClipData c = test::solidClip(tl, QColor::fromHsv(i % 360, 200, 200), i * 10, 10);
            c.transform.values[keys::Opacity].setKey(0, 0.5);
            c.transform.values[keys::Opacity].setKey(9, 1.0);
            test::addClip(tl, TrackType::Video, i % 2, c);
        }
        project.endEdit();
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("big.kkproj"));
        QElapsedTimer t;
        t.start();
        QVERIFY(project.save(path));
        const qint64 saveMs = t.elapsed();
        t.restart();
        Project loaded;
        QVERIFY(loaded.load(path));
        const qint64 loadMs = t.elapsed();
        qInfo("2000 clips: save %lld ms, load %lld ms, file %lld KiB", saveMs, loadMs,
            QFileInfo(path).size() / 1024);
        size_t count = 0;
        for (const TrackData& track : loaded.timeline()->data().videoTracks)
            count += track.clips.size();
        QCOMPARE(count, size_t(kClips));
        // Generous bounds (ASan builds are ~3x slower); typical numbers are in the log.
        QVERIFY(saveMs < 3000);
        QVERIFY(loadMs < 3000);
    }
};

QTEST_MAIN(TestRobustness)
#include "test_robustness.moc"
