// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for export/Otio (spec 51.1 - 51.3): a known timeline written as .otio and read back
// keeps its tracks, clips, gaps, transition, speed and markers; hand-written files as other
// tools write them (Clip.1 / Clip.2, rates 24, 25, 29.97, 30, unknown schemas) are read.
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "export/Otio.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace kk;
namespace ops = editops;

namespace {

ClipData mediaClip(
    const QString& id, const QString& name, int64_t start, int64_t duration, int64_t sourceSec)
{
    ClipData c;
    c.kind = ClipKind::Media;
    c.mediaId = id;
    c.name = name;
    c.start = start;
    c.duration = duration;
    c.sourceInUs = sourceSec * 1000000;
    c.sourceDurationUs = 600 * 1000000LL;
    return c;
}

/// Media of a fixture: the ids the importer's callback hands out, by path, and the calls made.
struct Media
{
    QHash<QString, QString> idByPath;
    QStringList calls;
    otio::ImportOptions options(Rational fps = {24, 1})
    {
        otio::ImportOptions o;
        o.baseDir = QStringLiteral("/proj/edit");
        o.defaultFps = fps;
        o.resolveMedia = [this](const otio::MediaRef& ref) {
            calls << (ref.path.isEmpty() ? QStringLiteral("name:") + ref.name : ref.path);
            const QString key = ref.path.isEmpty() ? ref.name : ref.path;
            if (!idByPath.contains(key))
                idByPath.insert(key, QStringLiteral("m%1").arg(idByPath.size() + 1));
            return otio::ResolvedMedia {idByPath.value(key), ref.durationUs};
        };
        return o;
    }
};

QString rt(double value, double rate)
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"RationalTime.1","rate":%2,"value":%1})")
        .arg(value)
        .arg(rate);
}

QString range(double start, double duration, double rate)
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"TimeRange.1","start_time":%1,"duration":%2})")
        .arg(rt(start, rate), rt(duration, rate));
}

/// Clip.1 as older tools write it: one media_reference with an available range.
QString clip1(const QString& name, const QString& url, double srcStart, double srcDur,
    double availStart, double availDur, double rate, const QString& markers = QStringLiteral("[]"))
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"Clip.1","metadata":{},"name":"%1","source_range":%2,
        "effects":[],"markers":%5,"enabled":true,
        "media_reference":{"OTIO_SCHEMA":"ExternalReference.1","metadata":{},"name":"",
            "available_range":%3,"target_url":"%4"}})")
        .arg(name, range(srcStart, srcDur, rate), range(availStart, availDur, rate), url, markers);
}

/// Clip.2 as current OpenTimelineIO writes it.
QString clip2(const QString& name, const QString& url, double srcStart, double srcDur,
    double availStart, double availDur, double rate, const QString& effects = QStringLiteral("[]"))
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"Clip.2","metadata":{},"name":"%1","source_range":%2,
        "effects":%5,"markers":[],"enabled":true,"active_media_reference_key":"DEFAULT_MEDIA",
        "media_references":{"DEFAULT_MEDIA":{"OTIO_SCHEMA":"ExternalReference.1","metadata":{},
            "name":"","available_range":%3,"available_image_bounds":null,"target_url":"%4"}}})")
        .arg(name, range(srcStart, srcDur, rate), range(availStart, availDur, rate), url, effects);
}

QString gap(double dur, double rate)
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"Gap.1","metadata":{},"name":"","source_range":%1,
        "effects":[],"markers":[],"enabled":true})")
        .arg(range(0, dur, rate));
}

QString track(const QString& kind, const QStringList& children)
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"Track.1","metadata":{},"name":"","kind":"%1",
        "source_range":null,"effects":[],"markers":[],"enabled":true,"children":[%2]})")
        .arg(kind, children.join(QLatin1Char(',')));
}

QString timeline(const QStringList& tracks, const QString& globalStart = QStringLiteral("null"),
    const QString& stackMarkers = QStringLiteral("[]"))
{
    return QStringLiteral(R"({"OTIO_SCHEMA":"Timeline.1","metadata":{},"name":"Fixture",
        "global_start_time":%1,"tracks":{"OTIO_SCHEMA":"Stack.1","metadata":{},"name":"tracks",
        "source_range":null,"effects":[],"markers":%3,"enabled":true,"children":[%2]}})")
        .arg(globalStart, tracks.join(QLatin1Char(',')), stackMarkers);
}

otio::TimelineImport read(const QString& text, Media& media, Rational fps = {24, 1})
{
    return otio::importOtio(text.toUtf8(), media.options(fps));
}

} // namespace

class TestOtio : public QObject
{
    Q_OBJECT
private slots:
    void roundTripKeepsEverything_data()
    {
        QTest::addColumn<int>("rateNum");
        QTest::addColumn<int>("rateDen");
        QTest::addColumn<bool>("legacy");
        QTest::newRow("25 Clip.2") << 25 << 1 << false;
        QTest::newRow("29.97 Clip.2") << 30000 << 1001 << false;
        QTest::newRow("24 Clip.1") << 24 << 1 << true;
    }

    void roundTripKeepsEverything()
    {
        QFETCH(int, rateNum);
        QFETCH(int, rateDen);
        QFETCH(bool, legacy);
        const Rational fps {rateNum, rateDen};

        MediaPool pool;
        MediaItem a;
        a.path = QStringLiteral("/m/Interview A.mov");
        a.name = QStringLiteral("Interview A.mov");
        a.info.valid = a.info.hasVideo = a.info.hasAudio = true;
        a.info.durationUs = 600 * 1000000LL;
        const QString ida = pool.addItem(a);
        MediaItem b;
        b.path = QStringLiteral("/m/broll.mp4");
        b.name = QStringLiteral("broll.mp4");
        b.info.valid = b.info.hasVideo = true;
        b.info.durationUs = 600 * 1000000LL;
        const QString idb = pool.addItem(b);

        TimelineData d = makeDefaultTimeline(QStringLiteral("Cut 1"), fps, 1920, 1080, 2, 2);
        d.startTimecode = oneHourTimecode(fps);
        // V1: A [0, 100), B [100, 200) with a 20-frame centred dissolve, a gap, B at half
        // speed [250, 300), a reversed clip, a frozen one. V2: a title. A1: A's audio with a
        // gap before; A2 muted.
        ops::overwriteClip(
            d, {TrackType::Video, 0}, mediaClip(ida, QStringLiteral("Interview A"), 0, 100, 10));
        ClipData second = mediaClip(idb, QStringLiteral("B-roll"), 100, 100, 20);
        second.transitionIn
            = EffectRegistry::instance().createTransition(QStringLiteral("cross_dissolve"), 20);
        ClipData withMarker = mediaClip(idb, QStringLiteral("B-roll 2"), 300, 50, 33);
        Marker clipMarker;
        clipMarker.frame = 10;
        clipMarker.name = QStringLiteral("In clip");
        clipMarker.note = QStringLiteral("remember");
        clipMarker.color = QColor(0xe0, 0x4a, 0x4a);
        withMarker.markers = {clipMarker};
        ClipData slow = mediaClip(idb, QStringLiteral("B-roll slow"), 250, 50, 40);
        slow.speed = 0.5;
        ClipData reverse = mediaClip(idb, QStringLiteral("Reverse"), 350, 25, 60);
        reverse.speed = -1.0;
        ClipData freeze = mediaClip(idb, QStringLiteral("Freeze"), 375, 25, 61);
        freeze.speed = 0.0;
        freeze.enabled = false;
        for (const ClipData& c : {second, slow, withMarker, reverse, freeze})
            ops::overwriteClip(d, {TrackType::Video, 0}, c);
        ClipData title;
        title.kind = ClipKind::Title;
        title.name = QStringLiteral("Title");
        title.start = 10;
        title.duration = 40;
        ops::overwriteClip(d, {TrackType::Video, 1}, title);
        ops::overwriteClip(
            d, {TrackType::Audio, 0}, mediaClip(ida, QStringLiteral("Interview A"), 25, 75, 10));
        d.audioTracks[1].muted = true;
        d.audioTracks[0].name = QStringLiteral("Dialogue");
        Marker tm;
        tm.frame = 50;
        tm.duration = 25;
        tm.name = QStringLiteral("Chapter");
        tm.note = QStringLiteral("line 1");
        tm.color = QColor(0xc8, 0xb0, 0x84); // sand: no OTIO colour of its own
        d.markers = {tm};

        otio::ExportOptions xo;
        xo.legacyClipSchema = legacy;
        xo.sourceStartUs = [](const ClipData&) { return int64_t(3600) * 1000000; };
        const QByteArray file = otio::timelineToOtio(d, &pool, xo);

        // The file is plain OTIO JSON.
        const QJsonObject root = QJsonDocument::fromJson(file).object();
        QCOMPARE(root.value("OTIO_SCHEMA").toString(), QStringLiteral("Timeline.1"));
        const QJsonObject stack = root.value("tracks").toObject();
        QCOMPARE(stack.value("OTIO_SCHEMA").toString(), QStringLiteral("Stack.1"));
        QCOMPARE(stack.value("children").toArray().size(), 4);
        const QJsonObject v1 = stack.value("children").toArray()[0].toObject();
        QCOMPARE(v1.value("kind").toString(), QStringLiteral("Video"));
        QStringList schemas;
        for (const QJsonValue& c : v1.value("children").toArray())
            schemas << c.toObject().value("OTIO_SCHEMA").toString();
        const QString clipSchema = legacy ? QStringLiteral("Clip.1") : QStringLiteral("Clip.2");
        QCOMPARE(schemas,
            (QStringList {clipSchema, QStringLiteral("Transition.1"), clipSchema,
                QStringLiteral("Gap.1"), clipSchema, clipSchema, clipSchema, clipSchema}));

        otio::ImportOptions io;
        io.defaultFps = {24, 1};
        io.resolveMedia = [&](const otio::MediaRef& ref) {
            return otio::ResolvedMedia {ref.path == QStringLiteral("/m/Interview A.mov") ? ida
                    : ref.path == QStringLiteral("/m/broll.mp4")                         ? idb
                                                                 : QStringLiteral("other"),
                600 * 1000000LL};
        };
        const otio::TimelineImport r = otio::importOtio(file, io);
        QVERIFY2(r.ok, qPrintable(r.error));
        const TimelineData& t = r.timeline;
        QCOMPARE(t.name, QStringLiteral("Cut 1"));
        QCOMPARE(t.fps, fps);
        QCOMPARE(t.startTimecode, d.startTimecode);
        QCOMPARE(t.videoTracks.size(), size_t(2));
        QCOMPARE(t.audioTracks.size(), size_t(2));
        QCOMPARE(t.audioTracks[0].name, QStringLiteral("Dialogue"));
        QVERIFY(t.audioTracks[1].muted);
        QVERIFY2(r.skipped.isEmpty(), qPrintable(r.skipped.join(QLatin1Char(';'))));

        for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
            const auto& want = d.tracks(type);
            const auto& got = t.tracks(type);
            for (size_t i = 0; i < want.size(); ++i) {
                QCOMPARE(got[i].clips.size(), want[i].clips.size());
                for (size_t k = 0; k < want[i].clips.size(); ++k) {
                    const ClipData& w = want[i].clips[k];
                    const ClipData& g = got[i].clips[k];
                    const QByteArray what = QStringLiteral("%1 track %2 clip %3")
                                                .arg(int(type))
                                                .arg(i)
                                                .arg(k)
                                                .toLatin1();
                    QVERIFY2(g.start == w.start && g.duration == w.duration, what);
                    QVERIFY2(g.sourceInUs == w.sourceInUs || w.kind != ClipKind::Media, what);
                    QVERIFY2(qFuzzyCompare(g.speed + 1.0, w.speed + 1.0), what);
                    QVERIFY2(g.enabled == w.enabled, what);
                    QVERIFY2(g.name == w.name, what);
                    QVERIFY2(g.transitionIn.has_value() == w.transitionIn.has_value(), what);
                    if (w.transitionIn) {
                        QCOMPARE(g.transitionIn->duration, w.transitionIn->duration);
                        QCOMPARE(g.transitionIn->type, w.transitionIn->type);
                        QCOMPARE(g.transitionIn->alignment, w.transitionIn->alignment);
                    }
                    QCOMPARE(g.markers.size(), w.markers.size());
                    for (qsizetype m = 0; m < w.markers.size(); ++m) {
                        QCOMPARE(g.markers[m].frame, w.markers[m].frame);
                        QCOMPARE(g.markers[m].name, w.markers[m].name);
                        QCOMPARE(g.markers[m].note, w.markers[m].note);
                        QCOMPARE(g.markers[m].color, w.markers[m].color);
                    }
                    if (w.kind == ClipKind::Media)
                        QVERIFY2(g.mediaId == w.mediaId, what);
                }
            }
        }
        // The title has no media of its own: it comes back as a named offline clip.
        QCOMPARE(t.videoTracks[1].clips[0].name, QStringLiteral("Title"));
        QCOMPARE(t.markers.size(), 1);
        QCOMPARE(t.markers[0].frame, 50);
        QCOMPARE(t.markers[0].duration, 25);
        QCOMPARE(t.markers[0].name, QStringLiteral("Chapter"));
        QCOMPARE(t.markers[0].note, QStringLiteral("line 1"));
        QCOMPARE(t.markers[0].color, tm.color);
    }

    void exportWritesTheDocumentedShape()
    {
        MediaPool pool;
        MediaItem a;
        a.path = QStringLiteral("/m/My clip #1.mov");
        a.name = QStringLiteral("My clip #1.mov");
        a.info.valid = a.info.hasVideo = true;
        a.info.durationUs = 10 * 1000000LL;
        const QString ida = pool.addItem(a);
        TimelineData d = makeDefaultTimeline(QStringLiteral("T"), {25, 1}, 1920, 1080, 1, 1);
        ClipData c = mediaClip(ida, QStringLiteral("Clip"), 25, 50, 2);
        ops::overwriteClip(d, {TrackType::Video, 0}, c);
        const QJsonObject root = QJsonDocument::fromJson(otio::timelineToOtio(d, &pool)).object();
        const QJsonArray children
            = root["tracks"].toObject()["children"].toArray()[0].toObject()["children"].toArray();
        QCOMPARE(children.size(), 2);
        QCOMPARE(children[0].toObject()["OTIO_SCHEMA"].toString(), QStringLiteral("Gap.1"));
        QCOMPARE(children[0]
                     .toObject()["source_range"]
                     .toObject()["duration"]
                     .toObject()["value"]
                     .toDouble(),
            25.0);
        const QJsonObject clip = children[1].toObject();
        QCOMPARE(clip["OTIO_SCHEMA"].toString(), QStringLiteral("Clip.2"));
        QCOMPARE(clip["active_media_reference_key"].toString(), QStringLiteral("DEFAULT_MEDIA"));
        const QJsonObject ref = clip["media_references"].toObject()["DEFAULT_MEDIA"].toObject();
        QCOMPARE(ref["OTIO_SCHEMA"].toString(), QStringLiteral("ExternalReference.1"));
        QCOMPARE(ref["target_url"].toString(), QStringLiteral("file:///m/My%20clip%20%231.mov"));
        const QJsonObject source = clip["source_range"].toObject();
        QCOMPARE(source["start_time"].toObject()["value"].toDouble(), 50.0); // 2 s at 25
        QCOMPARE(source["start_time"].toObject()["rate"].toDouble(), 25.0);
        QCOMPARE(source["duration"].toObject()["value"].toDouble(), 50.0);
        QCOMPARE(
            ref["available_range"].toObject()["duration"].toObject()["value"].toDouble(), 250.0);
    }

    void clip1AtTwentyFourFrames()
    {
        Media media;
        const QString text
            = timeline({track("Video",
                            {clip1("A", "file:///m/a.mov", 240, 48, 0, 2400, 24), gap(24, 24),
                                clip1("B", "file:///m/b%20x.mov", 0, 24, 0, 480, 24)}),
                           track("Audio", {clip1("A", "file:///m/a.mov", 240, 48, 0, 2400, 24)})},
                rt(86400, 24));
        const otio::TimelineImport r = read(text, media);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.timeline.fps, (Rational {24, 1}));
        QCOMPARE(r.timeline.startTimecode, int64_t(86400));
        const auto& clips = r.timeline.videoTracks[0].clips;
        QCOMPARE(clips.size(), size_t(2));
        QCOMPARE(clips[0].start, int64_t(0));
        QCOMPARE(clips[0].duration, int64_t(48));
        QCOMPARE(clips[0].sourceInUs, int64_t(10000000));
        QCOMPARE(clips[1].start, int64_t(72));
        QCOMPARE(clips[1].duration, int64_t(24));
        QCOMPARE(clips[1].sourceInUs, int64_t(0));
        QCOMPARE(r.timeline.audioTracks[0].clips[0].mediaId, clips[0].mediaId);
        QVERIFY(clips[0].mediaId != clips[1].mediaId);
        // One call per distinct file, with the decoded path and the available length.
        QCOMPARE(
            media.calls, (QStringList {QStringLiteral("/m/a.mov"), QStringLiteral("/m/b x.mov")}));
        QCOMPARE(clips[0].sourceDurationUs, int64_t(100000000));
        QCOMPARE(r.clipCount, 3);
    }

    void clip2AtTwentyFiveFramesWithSpeed()
    {
        Media media;
        const QString warp = QStringLiteral(
            R"([{"OTIO_SCHEMA":"LinearTimeWarp.1","metadata":{},"name":"","effect_name":"LinearTimeWarp","time_scalar":2.0}])");
        const QString text = timeline({track("Video",
            {clip2("A", "file:///m/a.mov", 50, 100, 0, 5000, 25, warp),
                clip2("B", "file:///m/a.mov", 0, 25, 0, 5000, 25)})});
        const otio::TimelineImport r = read(text, media);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(
            r.timeline.fps, (Rational {25, 1})); // from the clips: no global_start_time, no gap
        const auto& clips = r.timeline.videoTracks[0].clips;
        QCOMPARE(clips[0].speed, 2.0);
        QCOMPARE(clips[0].duration, int64_t(100));
        QCOMPARE(clips[0].sourceInUs, int64_t(2000000));
        QCOMPARE(clips[1].start, int64_t(100));
        QCOMPARE(media.calls.size(), 1); // the same file twice
        QCOMPARE(clips[0].mediaId, clips[1].mediaId);
    }

    void ntscAndThirtyFrameRates()
    {
        // 29.97: 300 frames of a clip at the rounded rate 29.97 are 300 frames of 30000/1001.
        Media m1;
        otio::TimelineImport r = read(
            timeline({track("Video", {clip1("A", "file:///m/a.mov", 30, 300, 0, 9000, 29.97)})},
                rt(107892, 29.97)),
            m1);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.timeline.fps, (Rational {30000, 1001}));
        QCOMPARE(r.timeline.videoTracks[0].clips[0].duration, int64_t(300));
        QCOMPARE(r.timeline.videoTracks[0].clips[0].sourceInUs, int64_t(1001000)); // 30 frames
        // The exact rate (30000/1001 written out) gives the same.
        Media m2;
        r = read(timeline({track("Video",
                     {clip1("A", "file:///m/a.mov", 30, 300, 0, 9000, 29.97002997002997)})}),
            m2);
        QCOMPARE(r.timeline.fps, (Rational {30000, 1001}));
        QCOMPARE(r.timeline.videoTracks[0].clips[0].duration, int64_t(300));
        // 30 and 23.976.
        Media m3;
        r = read(
            timeline({track("Video", {clip2("A", "file:///m/a.mov", 0, 90, 0, 900, 30)})}), m3);
        QCOMPARE(r.timeline.fps, (Rational {30, 1}));
        QCOMPARE(r.timeline.videoTracks[0].clips[0].duration, int64_t(90));
        Media m4;
        r = read(
            timeline({track("Video", {clip2("A", "file:///m/a.mov", 0, 240, 0, 900, 23.976)})}),
            m4);
        QCOMPARE(r.timeline.fps, (Rational {24000, 1001}));
        QCOMPARE(r.timeline.videoTracks[0].clips[0].duration, int64_t(240));
    }

    void mediaRateDiffersFromTimelineRate()
    {
        // A 25 fps timeline (global start and gaps at 25) holding a clip whose media is 24 fps:
        // 48 frames of media are two seconds = 50 timeline frames.
        Media media;
        const QString text = timeline(
            {track("Video", {gap(25, 25), clip1("A", "file:///m/a.mov", 24, 48, 0, 2400, 24)})},
            rt(90000, 25));
        const otio::TimelineImport r = read(text, media);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.timeline.fps, (Rational {25, 1}));
        const ClipData& c = r.timeline.videoTracks[0].clips[0];
        QCOMPARE(c.start, int64_t(25));
        QCOMPARE(c.duration, int64_t(50));
        QCOMPARE(c.sourceInUs, int64_t(1000000));
    }

    void unknownThingsAreSkippedAndReported()
    {
        Media media;
        const QString unknownEffect = QStringLiteral(
            R"([{"OTIO_SCHEMA":"Effect.1","metadata":{},"name":"blur","effect_name":"Blur"},)"
            R"({"OTIO_SCHEMA":"Effect.1","metadata":{},"name":"blur","effect_name":"Blur"}])");
        const QString wipe = QStringLiteral(
            R"({"OTIO_SCHEMA":"Transition.1","metadata":{},"name":"","in_offset":%1,"out_offset":%2,"transition_type":"SMPTE_Wipe"})")
                                 .arg(rt(5, 24), rt(5, 24));
        const QString text = timeline(
            {track("Video",
                 {clip1("A", "file:///m/a.mov", 0, 48, 0, 480, 24), wipe,
                     clip2("B", "file:///m/b.mov", 0, 48, 0, 480, 24, unknownEffect),
                     R"({"OTIO_SCHEMA":"Hologram.7","name":"?","source_range":)" + range(0, 24, 24)
                         + "}",
                     clip2("C", "file:///m/c.mov", 0, 24, 0, 480, 24)}),
                track("Subtitles", {gap(10, 24)}), R"({"OTIO_SCHEMA":"Stack.1","children":[]})"},
            rt(86400, 24));
        const otio::TimelineImport r = read(text, media);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.timeline.videoTracks.size(), size_t(1));
        const auto& clips = r.timeline.videoTracks[0].clips;
        QCOMPARE(clips.size(), size_t(3));
        QCOMPARE(clips[2].start, int64_t(120)); // the unknown item kept its 24 frames as a gap
        // The wipe came in as a dissolve of 10 frames on B.
        QVERIFY(clips[1].transitionIn.has_value());
        QCOMPARE(clips[1].transitionIn->type, QStringLiteral("cross_dissolve"));
        QCOMPARE(clips[1].transitionIn->duration, int64_t(10));
        const QString all = r.skipped.join(QLatin1Char('\n'));
        QVERIFY2(all.contains(QStringLiteral("Effect.1 (2 times)")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("Hologram.7")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("SMPTE_Wipe")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("Subtitles")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("Stack.1")), qPrintable(all));
    }

    void metadataIsReported()
    {
        Media media;
        const QString text
            = QStringLiteral(
                  R"({"OTIO_SCHEMA":"Timeline.1","metadata":{"resolve":{"a":1}},"name":"M","global_start_time":null,)"
                  R"("tracks":{"OTIO_SCHEMA":"Stack.1","children":[)")
            + track("Video",
                {QStringLiteral(
                     R"({"OTIO_SCHEMA":"Clip.2","metadata":{"Resolve_OTIO":{"x":1}},"name":"A","source_range":)")
                    + range(0, 24, 24)
                    + R"(,"media_references":{},"active_media_reference_key":"DEFAULT_MEDIA"})"})
            + "]}}";
        const otio::TimelineImport r = read(text, media);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.skipped.contains(QStringLiteral("metadata of Timeline.1")));
        QVERIFY(r.skipped.contains(QStringLiteral("metadata of Clip.2")));
        // A clip with no reference at all is an offline clip called after the clip.
        QCOMPARE(media.calls, (QStringList {QStringLiteral("name:A")}));
        QCOMPARE(r.timeline.videoTracks[0].clips.size(), size_t(1));
    }

    void missingReferenceAndRelativePath()
    {
        Media media;
        const QString missing = QStringLiteral(
            R"({"OTIO_SCHEMA":"Clip.1","metadata":{},"name":"Gone","source_range":%1,"effects":[],"markers":[],"enabled":true,)"
            R"("media_reference":{"OTIO_SCHEMA":"MissingReference.1","metadata":{},"name":"","available_range":null}})")
                                    .arg(range(0, 24, 24));
        const QString text = timeline({track("Video",
            {clip1("Rel", "media/rel.mov", 0, 24, 0, 240, 24), missing,
                clip1("Web", "https://example.com/x.mov", 0, 24, 0, 240, 24)})});
        const otio::TimelineImport r = read(text, media);
        QVERIFY2(r.ok, qPrintable(r.error));
        // Relative targets resolve against the folder of the .otio file; remote ones are offline.
        QCOMPARE(media.calls,
            (QStringList {QStringLiteral("/proj/edit/media/rel.mov"), QStringLiteral("name:Gone"),
                QStringLiteral("name:Web")}));
        QCOMPARE(r.timeline.videoTracks[0].clips.size(), size_t(3));
    }

    void fadeNextToAGap()
    {
        Media media;
        const QString fadeIn = QStringLiteral(
            R"({"OTIO_SCHEMA":"Transition.1","metadata":{},"name":"","in_offset":%1,"out_offset":%2,"transition_type":"SMPTE_Dissolve"})")
                                   .arg(rt(0, 24), rt(12, 24));
        const QString text = timeline({track(
            "Video", {gap(24, 24), fadeIn, clip1("A", "file:///m/a.mov", 0, 48, 0, 480, 24)})});
        const otio::TimelineImport r = read(text, media);
        QVERIFY2(r.ok, qPrintable(r.error));
        const ClipData& c = r.timeline.videoTracks[0].clips[0];
        QCOMPARE(c.start, int64_t(24));
        QVERIFY(c.transitionIn.has_value());
        QCOMPARE(c.transitionIn->duration, int64_t(12));
        QCOMPARE(c.transitionIn->alignment, TransitionAlignment::StartOnCut);
    }

    void markersOnTheStackAndOnClips()
    {
        Media media;
        const QString marker = QStringLiteral(
            R"({"OTIO_SCHEMA":"Marker.2","metadata":{},"name":"M1","color":"RED","comment":"c","marked_range":%1})")
                                   .arg(range(86400 + 48, 1, 24));
        const QString clipMarker = QStringLiteral(
            R"([{"OTIO_SCHEMA":"Marker.1","metadata":{},"name":"inside","color":"GREEN","marked_range":%1}])")
                                       .arg(range(240 + 12, 1, 24));
        const QString fixed = timeline(
            {track("Video", {clip1("A", "file:///m/a.mov", 240, 48, 0, 2400, 24, clipMarker)})},
            rt(86400, 24), QStringLiteral("[%1]").arg(marker));
        const otio::TimelineImport r = read(fixed, media);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.timeline.markers.size(), 1);
        QCOMPARE(r.timeline.markers[0].frame, int64_t(48)); // relative to the starting timecode
        QCOMPARE(r.timeline.markers[0].name, QStringLiteral("M1"));
        QCOMPARE(r.timeline.markers[0].note, QStringLiteral("c"));
        QCOMPARE(r.timeline.markers[0].color, otio::markerColorFromOtio(QStringLiteral("RED")));
        const ClipData& c = r.timeline.videoTracks[0].clips[0];
        QCOMPARE(c.markers.size(), 1);
        QCOMPARE(c.markers[0].frame, int64_t(12));
        QCOMPARE(c.markers[0].name, QStringLiteral("inside"));
    }

    void notOtioIsRefused()
    {
        Media media;
        otio::TimelineImport r = otio::importOtio("not json", media.options());
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
        r = otio::importOtio(R"({"OTIO_SCHEMA":"Clip.2","name":"x"})", media.options());
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("Clip.2")));
        // A collection: the first timeline is read, the others reported.
        const QString inner
            = timeline({track("Video", {clip1("A", "file:///m/a.mov", 0, 24, 0, 240, 24)})});
        r = read(QStringLiteral(
                     R"({"OTIO_SCHEMA":"SerializableCollection.1","name":"c","children":[%1,%1]})")
                     .arg(inner),
            media);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.timeline.videoTracks[0].clips.size(), size_t(1));
        QVERIFY(r.skipped.join(QLatin1Char(' ')).contains(QStringLiteral("1 more timelines")));
    }

    void rateSnapping()
    {
        QCOMPARE(otio::rateToRational(24), (Rational {24, 1}));
        QCOMPARE(otio::rateToRational(23.976), (Rational {24000, 1001}));
        QCOMPARE(otio::rateToRational(23.976023976023978), (Rational {24000, 1001}));
        QCOMPARE(otio::rateToRational(29.97), (Rational {30000, 1001}));
        QCOMPARE(otio::rateToRational(59.94), (Rational {60000, 1001}));
        QCOMPARE(otio::rateToRational(25), (Rational {25, 1}));
        QCOMPARE(otio::rateToRational(12.5), (Rational {12500, 1000}));
        QVERIFY(!otio::rateToRational(0).isValid());
    }
};

QTEST_MAIN(TestOtio)
#include "test_otio.moc"
