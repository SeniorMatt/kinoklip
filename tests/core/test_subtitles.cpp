// SPDX-License-Identifier: GPL-3.0-or-later
// Subtitles in core: SRT / WebVTT parsing and writing (UTF-8, BOM, CRLF, multi-line, markup,
// overlapping cues, ',' and '.' separators), cue <-> caption conversion, editing captions on
// subtitle tracks, and serialization + migration of subtitle tracks, adjustment and generator
// clips (project format 7).
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/Generators.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Subtitles.h"
#include "core/Timeline.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;
namespace sub = kk::subtitles;

namespace {

constexpr TrackRef kST1 {TrackType::Subtitle, 0};

/// Timeline with V1, V2, A1, A2 and one subtitle track holding captions "a" [0, 10),
/// "b" [20, 30), "c" [40, 50).
TimelineData withCaptions()
{
    TimelineData tl = timeline();
    editops::addTrack(tl, TrackType::Subtitle);
    for (const auto& [text, start] : {std::pair {"a", 0}, {"b", 20}, {"c", 40}})
        editops::overwriteClip(tl, kST1, sub::makeCaption(QString::fromUtf8(text), start, 10));
    return tl;
}

QStringList texts(const TimelineData& tl, TrackRef ref = kST1)
{
    QStringList out;
    for (const ClipData& c : tl.track(ref)->clips)
        out.append(sub::captionText(c));
    return out;
}

} // namespace

class TestSubtitles : public QObject
{
    Q_OBJECT

private slots:
    // ---- Parsing ----

    void parseSrt()
    {
        // BOM, CRLF, comma separators, multi-line text, UTF-8, markup, an index-less cue and
        // a cue with a short millisecond field.
        const QByteArray srt = "\xef\xbb\xbf"
                               "1\r\n00:00:01,000 --> 00:00:02,500\r\nHello <i>world</i>\r\n\r\n"
                               "2\r\n00:00:03,000 --> 00:00:05,000\r\nLine one\r\n"
                               "{\\an8}Zeile zwei: \xc3\xa4\xc3\xb6\xc3\xbc \xe2\x80\x94 "
                               "\xe6\x97\xa5\xe6\x9c\xac\r\n\r\n"
                               "00:01:00.5 --> 01:00:00,000\r\nno index, dot\r\n";
        QVector<SubtitleCue> cues;
        SubtitleFormat format = SubtitleFormat::WebVtt;
        QString error;
        QVERIFY2(sub::parse(srt, &cues, &error, &format), qPrintable(error));
        QCOMPARE(format, SubtitleFormat::Srt);
        QCOMPARE(cues.size(), 3);
        QCOMPARE(cues[0], (SubtitleCue {1'000'000, 2'500'000, QStringLiteral("Hello world")}));
        QCOMPARE(cues[1].text, QStringLiteral("Line one\nZeile zwei: äöü — 日本"));
        QCOMPARE(cues[2].startUs, 60'500'000);
        QCOMPARE(cues[2].endUs, 3600'000'000LL);
    }

    // Spec 28.18: subtitle files in UTF-16 (with or without BOM) and in Windows-1252 /
    // Latin-1 keep their accented letters (they used to become U+FFFD or nothing parsed).
    void parseOtherEncodings()
    {
        const QString text
            = QStringLiteral("1\n00:00:01,000 --> 00:00:02,000\nCaf\u00e9 \u00e0 la cr\u00e8me\n");
        const QString expected = QStringLiteral("Caf\u00e9 \u00e0 la cr\u00e8me");
        QVector<QByteArray> files;
        files.append(text.toLatin1()); // 8-bit, not UTF-8
        QByteArray le("\xFF\xFE", 2);
        for (QChar c : text)
            le.append(char(c.unicode() & 0xff)).append(char(c.unicode() >> 8));
        files.append(le); // UTF-16 LE with BOM
        files.append(le.mid(2)); // without BOM
        QByteArray be("\xFE\xFF", 2);
        for (QChar c : text)
            be.append(char(c.unicode() >> 8)).append(char(c.unicode() & 0xff));
        files.append(be);
        for (int i = 0; i < files.size(); ++i) {
            QVector<SubtitleCue> cues;
            QString error;
            QVERIFY2(sub::parse(files[i], &cues, &error), qPrintable(QString::number(i) + error));
            QCOMPARE(cues.size(), 1);
            QCOMPARE(cues[0].text, expected);
        }
    }

    void parseWebVtt()
    {
        const QByteArray vtt = "WEBVTT - a title\n\n"
                               "NOTE this is a comment\nspanning lines\n\n"
                               "STYLE\n::cue { color: yellow }\n\n"
                               "intro\n00:01.000 --> 00:02.000 line:90% align:center\n"
                               "<v Roger>Hi &amp; welcome &lt;3</v>\n\n"
                               "00:00:02.000 --> 00:00:04.000\n<c.yellow>two</c>\nlines\n";
        QVector<SubtitleCue> cues;
        SubtitleFormat format = SubtitleFormat::Srt;
        QVERIFY(sub::parse(vtt, &cues, nullptr, &format));
        QCOMPARE(format, SubtitleFormat::WebVtt);
        QCOMPARE(cues.size(), 2);
        QCOMPARE(cues[0], (SubtitleCue {1'000'000, 2'000'000, QStringLiteral("Hi & welcome <3")}));
        QCOMPARE(cues[1], (SubtitleCue {2'000'000, 4'000'000, QStringLiteral("two\nlines")}));
    }

    void rejectsGarbage()
    {
        QVector<SubtitleCue> cues;
        QString error;
        QVERIFY(!sub::parse("just some text\nwithout cues\n", &cues, &error));
        QVERIFY(!error.isEmpty());
        // A reversed / empty cue is skipped, the valid one kept.
        QVERIFY(sub::parse("1\n00:00:05,000 --> 00:00:04,000\nx\n\n"
                           "2\n00:00:01,000 --> 00:00:02,000\ny\n",
            &cues));
        QCOMPARE(cues.size(), 1);
        QCOMPARE(cues[0].text, QStringLiteral("y"));
    }

    // ---- Writing ----

    void timestamps()
    {
        QCOMPARE(sub::formatTimestamp(3'723'004'000LL, SubtitleFormat::Srt),
            QStringLiteral("01:02:03,004"));
        QCOMPARE(sub::formatTimestamp(1'999'600, SubtitleFormat::WebVtt),
            QStringLiteral("00:00:02.000")); // rounded to milliseconds
        QCOMPARE(sub::formatTimestamp(-5, SubtitleFormat::Srt), QStringLiteral("00:00:00,000"));
    }

    void roundTrip_data()
    {
        QTest::addColumn<int>("format");
        QTest::newRow("srt") << int(SubtitleFormat::Srt);
        QTest::newRow("vtt") << int(SubtitleFormat::WebVtt);
    }
    void roundTrip()
    {
        QFETCH(int, format);
        const auto f = SubtitleFormat(format);
        const QVector<SubtitleCue> cues {
            {0, 1'500'000, QStringLiteral("First")},
            {1'000'000, 3'000'000, QStringLiteral("Overlapping\ntwo lines")},
            {3'600'000'000LL, 3'601'234'000LL, QStringLiteral("Été <b> & 中文")},
        };
        const QByteArray bytes = sub::write(cues, f);
        if (f == SubtitleFormat::Srt) {
            QVERIFY(bytes.startsWith("1\n00:00:00,000 --> 00:00:01,500\nFirst\n\n2\n"));
        } else {
            QVERIFY(bytes.startsWith("WEBVTT\n\n00:00:00.000 --> 00:00:01.500\nFirst\n\n"));
            QVERIFY(bytes.contains("&lt;b&gt; &amp;")); // escaped in WebVTT
        }
        QVector<SubtitleCue> back;
        QVERIFY(sub::parse(bytes, &back));
        if (f == SubtitleFormat::Srt) {
            // SRT has no escaping: the "<b>" survives as text only through the tag filter,
            // which removes it (it looks like markup).
            QCOMPARE(back[2].text, QStringLiteral("Été  & 中文"));
            back[2].text = cues[2].text;
        }
        QCOMPARE(back, cues);

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("x.") + sub::suffix(f));
        QString error;
        QVERIFY(sub::writeFile(path, cues, f, &error));
        QVERIFY(sub::readFile(path, &back, &error));
        QCOMPARE(back.size(), 3);
        QVERIFY(sub::isSubtitleFile(path));
    }

    void blankLinesInTextAreDropped()
    {
        const QByteArray bytes
            = sub::write({{0, 1000, QStringLiteral("a\n\n  \nb")}, {2000, 3000, QString()}},
                SubtitleFormat::Srt);
        QCOMPARE(bytes, QByteArray("1\n00:00:00,000 --> 00:00:00,001\na\nb\n\n"));
    }

    // ---- Timeline conversion ----

    void cuesToCaptions()
    {
        // Overlapping cues go to further lanes; times round to the nearest frame.
        const QVector<SubtitleCue> cues {
            {1'000'000, 2'000'000, QStringLiteral("one")},
            {1'500'000, 2'500'000, QStringLiteral("overlap")},
            {2'000'000, 2'010'000, QStringLiteral("short")}, // < 1 frame -> 1 frame
            {2'600'000, 3'000'000, QStringLiteral("after")},
        };
        const auto placed = sub::captionsFromCues(cues, {25, 1}, 100);
        QCOMPARE(placed.size(), 4);
        QCOMPARE(placed[0].lane, 0);
        QCOMPARE(placed[0].clip.start, 125);
        QCOMPARE(placed[0].clip.duration, 25);
        QCOMPARE(placed[1].lane, 1);
        QCOMPARE(placed[2].lane, 0); // starts where "one" ends
        QCOMPARE(placed[2].clip.duration, 1);
        QCOMPARE(placed[3].lane, 0);
        QCOMPARE(placed[0].clip.kind, ClipKind::Subtitle);
        QCOMPARE(sub::captionText(placed[1].clip), QStringLiteral("overlap"));

        // Back to cues relative to a range, clipped to it.
        TimelineData tl = timeline();
        editops::addTrack(tl, TrackType::Subtitle);
        for (const auto& p : placed) {
            if (p.lane == 0)
                editops::overwriteClip(tl, kST1, p.clip);
        }
        const QVector<SubtitleCue> back
            = sub::cuesFromTrack(tl.subtitleTracks[0], {25, 1}, 130, 200);
        QCOMPARE(back.size(), 3);
        QCOMPARE(back[0], (SubtitleCue {0, 800'000, QStringLiteral("one")})); // clipped head
        QCOMPARE(back[2].startUs, 2'600'000 - 1'200'000);
    }

    // ---- Editing ----

    void editOps()
    {
        TimelineData tl = withCaptions();
        KK_VERIFY_INVARIANTS(tl);
        QCOMPARE(trackDisplayName(tl.subtitleTracks[0], 0), QStringLiteral("ST1"));
        QCOMPARE(tl.subtitleTracks[0].subtitleStyle, sub::defaultStyle());
        // Split keeps the text on both halves.
        const quint64 a = tl.subtitleTracks[0].clips[0].id;
        QCOMPARE(editops::splitClips(tl, {a}, 4).size(), 1);
        QCOMPARE(texts(tl), (QStringList {"a", "a", "b", "c"}));
        // Trim with ripple moves the later captions.
        const quint64 b = tl.subtitleTracks[0].clips[2].id;
        QCOMPARE(editops::trimTail(tl, b, 5, true), 5);
        QCOMPARE(layout(tl, kST1), QStringLiteral("0:4 4:6 20:15 45:10"));
        // Ripple delete closes the gap on the subtitle track.
        editops::rippleDeleteClips(tl, {b});
        QCOMPARE(layout(tl, kST1), QStringLiteral("0:4 4:6 30:10"));
        // Moving stays on subtitle tracks (and between them).
        editops::addTrack(tl, TrackType::Subtitle);
        const quint64 c = tl.subtitleTracks[0].clips.back().id;
        QVERIFY(editops::moveClips(tl, {c}, 10, 1));
        QCOMPARE(layout(tl, TrackRef {TrackType::Subtitle, 1}), QStringLiteral("40:10"));
        // An insert on V1 ripples the subtitle track along (all-track ripple keeps sync).
        editops::insertClip(tl, kV1, mediaClip(0, 5));
        QCOMPARE(layout(tl, kST1), QStringLiteral("5:4 9:6"));
        // Copy / paste returns captions to subtitle tracks.
        const auto board = editops::copyClips(tl, {tl.subtitleTracks[0].clips[0].id});
        const auto pasted = editops::pasteClips(tl, board, 100);
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(editops::findClip(tl, pasted[0])->track.type, TrackType::Subtitle);
        // Joining two halves of the same caption works; different texts do not join.
        const auto& st = tl.subtitleTracks[0].clips;
        QVERIFY(editops::joinClips(tl, st[0].id, st[1].id));
        KK_VERIFY_INVARIANTS(tl);
    }

    void invariantsRejectWrongTracks()
    {
        TimelineData tl = withCaptions();
        ClipData caption = sub::makeCaption(QStringLiteral("x"), 100, 5);
        caption.id = tl.allocateId();
        tl.videoTracks[0].clips.push_back(caption);
        QString why;
        QVERIFY(!editops::checkInvariants(tl, &why));
        QVERIFY(why.contains(QStringLiteral("wrong kind of track")));
    }

    // ---- Serialization ----

    void serializationRoundTrip()
    {
        TimelineData tl = withCaptions();
        tl.subtitleTracks[0].language = QStringLiteral("de");
        tl.subtitleTracks[0].subtitleStyle.setStatic(keys::Color, QColor(Qt::yellow));
        tl.subtitleTracks[0].clips[1].title.setBase(keys::Size, 80);
        ClipData adj;
        adj.kind = ClipKind::Adjustment;
        adj.start = 0;
        adj.duration = 50;
        EffectInstance blur;
        blur.id = tl.allocateId();
        blur.type = QStringLiteral("gaussian_blur");
        adj.effects.append(blur);
        editops::overwriteClip(tl, kV2, adj);
        ClipData gen;
        gen.kind = ClipKind::Generator;
        gen.generator = generators::Noise;
        gen.title.setBase(generators::Grain, 3);
        gen.start = 60;
        gen.duration = 20;
        editops::overwriteClip(tl, kV1, gen);

        const QJsonObject json = serialization::timelineToJson(tl);
        TimelineData back;
        QString error;
        QVERIFY2(serialization::timelineFromJson(json, back, &error), qPrintable(error));
        QCOMPARE(back, tl);
        QCOMPARE(back.videoTracks[1].clips[0].kind, ClipKind::Adjustment);
        QCOMPARE(back.videoTracks[0].clips[0].generator, generators::Noise);
        // A timeline without subtitle tracks writes no "subtitleTracks" (older files stay
        // byte-identical) and old JSON without it loads.
        QVERIFY(
            !serialization::timelineToJson(timeline()).contains(QStringLiteral("subtitleTracks")));
    }

    void projectFormatAndMigration()
    {
        QTemporaryDir dir;
        // A subtitle file imported into the media pool keeps its cues in the project.
        const QString srtPath = dir.filePath(QStringLiteral("captions.srt"));
        QString error;
        QVERIFY(sub::writeFile(srtPath,
            {{0, 1'000'000, QStringLiteral("eins")},
                {2'000'000, 3'000'000, QStringLiteral("zwei")}},
            SubtitleFormat::Srt, &error));
        Project p;
        const ImportResult imported = p.importMedia({srtPath});
        QCOMPARE(imported.importedIds.size(), 1);
        const MediaItem* item = p.mediaPool()->item(imported.importedIds[0]);
        QVERIFY(item && item->isSubtitle());
        QCOMPARE(item->cues.size(), 2);
        QCOMPARE(item->info.durationUs, 3'000'000);
        p.beginEdit(QStringLiteral("captions"));
        p.timeline()->mutableData() = withCaptions();
        p.endEdit();
        const QString path = dir.filePath(QStringLiteral("v7.kkproj"));
        QVERIFY(p.save(path, &error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QVERIFY(root.value(QStringLiteral("formatVersion")).toInt() >= 7); // 8 after multicam
        QFile::remove(srtPath); // the cues travel with the project
        Project q;
        QVERIFY2(q.load(path, &error), qPrintable(error));
        QCOMPARE(texts(q.timeline()->data()), (QStringList {"a", "b", "c"}));
        const MediaItem* loaded = q.mediaPool()->item(imported.importedIds[0]);
        QVERIFY(loaded && loaded->isSubtitle());
        QCOMPARE(loaded->cues, item->cues);

        // Versions 4 - 6 have none of the new fields and load unchanged.
        for (const int version : {4, 5, 6}) {
            QJsonObject old = root;
            old[QStringLiteral("formatVersion")] = version;
            QJsonArray timelines = old.value(QStringLiteral("timelines")).toArray();
            QJsonObject t = timelines[0].toObject();
            t.remove(QStringLiteral("subtitleTracks"));
            timelines[0] = t;
            old[QStringLiteral("timelines")] = timelines;
            const QString oldPath = dir.filePath(QStringLiteral("old%1.kkproj").arg(version));
            QFile out(oldPath);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(QJsonDocument(old).toJson());
            out.close();
            Project r;
            QVERIFY2(r.load(oldPath, &error), qPrintable(error));
            QVERIFY(r.timeline()->data().subtitleTracks.empty());
            QCOMPARE(r.timeline()->data().videoTracks.size(), size_t(2));
        }
    }
};

QTEST_GUILESS_MAIN(TestSubtitles)
#include "test_subtitles.moc"
