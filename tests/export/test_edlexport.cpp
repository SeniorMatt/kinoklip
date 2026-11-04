// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for export/EdlExport (spec 18.12, 18.13): a known timeline written as a CMX3600 EDL is
// parsed back field by field (events, channels, dissolves, M2 speed lines, source offsets),
// and the marker EDL / CSV lists.
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "export/EdlExport.h"

#include <QRegularExpression>
#include <QTest>

using namespace kk;
namespace ops = editops;

namespace {

/// One parsed event line of a CMX3600 EDL.
struct Line
{
    int number = 0;
    QString reel, channel, edit;
    int transition = 0;
    QString srcIn, srcOut, recIn, recOut;
};

/// Splits @p edl into event lines; comment / M2 lines go to @p others.
QVector<Line> parse(const QString& edl, QStringList* others = nullptr)
{
    static const QRegularExpression event(QStringLiteral(
        R"(^(\d{3})\s+(\S+)\s+(\S+)\s+(C|D)\s+(?:(\d{3})\s+)?(\S{11})\s+(\S{11})\s+(\S{11})\s+(\S{11})\s*$)"));
    QVector<Line> lines;
    for (const QString& text : edl.split(QLatin1Char('\n'))) {
        const auto m = event.match(text);
        if (!m.hasMatch()) {
            if (others && !text.trimmed().isEmpty())
                *others << text;
            continue;
        }
        Line l;
        l.number = m.captured(1).toInt();
        l.reel = m.captured(2);
        l.channel = m.captured(3);
        l.edit = m.captured(4);
        l.transition = m.captured(5).toInt();
        l.srcIn = m.captured(6);
        l.srcOut = m.captured(7);
        l.recIn = m.captured(8);
        l.recOut = m.captured(9);
        lines.append(l);
    }
    return lines;
}

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

} // namespace

class TestEdlExport : public QObject
{
    Q_OBJECT
private slots:
    void knownTimelineParsesBack()
    {
        MediaPool pool;
        MediaItem a;
        a.path = QStringLiteral("/m/Interview A.mov");
        a.info.valid = a.info.hasVideo = a.info.hasAudio = true;
        const QString ida = pool.addItem(a);
        MediaItem b;
        b.path = QStringLiteral("/m/broll_long_name_007.mp4");
        b.info.valid = b.info.hasVideo = true;
        const QString idb = pool.addItem(b);

        TimelineData d = makeDefaultTimeline(QStringLiteral("Cut 1"), {25, 1}, 1920, 1080, 2, 2);
        // V1: A [0, 100) from 10 s, B [100, 200) from 20 s with a 20-frame centred dissolve,
        // then B again at 50 % speed [250, 300) from 40 s (a gap before it).
        ops::overwriteClip(
            d, {TrackType::Video, 0}, mediaClip(ida, QStringLiteral("Interview A"), 0, 100, 10));
        ClipData second = mediaClip(idb, QStringLiteral("B-roll"), 100, 100, 20);
        second.transitionIn
            = EffectRegistry::instance().createTransition(QStringLiteral("cross_dissolve"), 20);
        ops::overwriteClip(d, {TrackType::Video, 0}, second);
        ClipData slow = mediaClip(idb, QStringLiteral("B-roll slow"), 250, 50, 40);
        slow.speed = 0.5;
        ops::overwriteClip(d, {TrackType::Video, 0}, slow);
        // A1: A's audio [0, 100); A2: music-less, empty.
        ops::overwriteClip(
            d, {TrackType::Audio, 0}, mediaClip(ida, QStringLiteral("Interview A"), 0, 100, 10));

        edl::EdlOptions options;
        // Interview A carries a start timecode of 01:00:00:00.
        options.sourceStartUs = [&ida](const ClipData& c) {
            return c.mediaId == ida ? int64_t(3600) * 1000000 : int64_t(0);
        };
        QStringList others;
        const QString text = edl::timelineToEdl(d, &pool, options);
        QVERIFY(text.startsWith(QStringLiteral("TITLE: Cut 1\nFCM: NON-DROP FRAME\n")));
        const QVector<Line> lines = parse(text, &others);
        QCOMPARE(lines.size(), 5);

        // 001 V: Interview A, cut, until the dissolve starts (10 frames before the cut).
        QCOMPARE(lines[0].number, 1);
        QCOMPARE(lines[0].reel, QStringLiteral("Intervie")); // 8 characters, spaces as '_'
        QCOMPARE(lines[0].channel, QStringLiteral("V"));
        QCOMPARE(lines[0].edit, QStringLiteral("C"));
        QCOMPARE(lines[0].srcIn, QStringLiteral("01:00:10:00"));
        QCOMPARE(lines[0].srcOut, QStringLiteral("01:00:13:15")); // 90 frames later
        QCOMPARE(lines[0].recIn, QStringLiteral("00:00:00:00"));
        QCOMPARE(lines[0].recOut, QStringLiteral("00:00:03:15"));
        // 002 A: the audio of A (no transition there): the whole clip.
        QCOMPARE(lines[1].number, 2);
        QCOMPARE(lines[1].channel, QStringLiteral("A"));
        QCOMPARE(lines[1].recOut, QStringLiteral("00:00:04:00"));
        // 003: dissolve. A zero-length cut of A at the dissolve start, then B with D 020.
        QCOMPARE(lines[2].number, 3);
        QCOMPARE(lines[2].reel, QStringLiteral("Intervie"));
        QCOMPARE(lines[2].srcIn, lines[2].srcOut);
        QCOMPARE(lines[2].recIn, QStringLiteral("00:00:03:15"));
        QCOMPARE(lines[2].recIn, lines[2].recOut);
        QCOMPARE(lines[3].number, 3);
        QCOMPARE(lines[3].reel, QStringLiteral("broll_lo"));
        QCOMPARE(lines[3].edit, QStringLiteral("D"));
        QCOMPARE(lines[3].transition, 20);
        QCOMPARE(lines[3].srcIn, QStringLiteral("00:00:19:15")); // 10 frames of handle
        QCOMPARE(lines[3].recIn, QStringLiteral("00:00:03:15"));
        QCOMPARE(lines[3].recOut, QStringLiteral("00:00:08:00"));
        // 004: the slow clip at its record position, source out = in + 50 x 0.5 frames.
        QCOMPARE(lines[4].number, 4);
        QCOMPARE(lines[4].recIn, QStringLiteral("00:00:10:00"));
        QCOMPARE(lines[4].recOut, QStringLiteral("00:00:12:00"));
        QCOMPARE(lines[4].srcIn, QStringLiteral("00:00:40:00"));
        QCOMPARE(lines[4].srcOut, QStringLiteral("00:00:41:00"));
        // The M2 line: reel, 12.5 fps (50 % of 25), source in.
        const QStringList m2 = others.filter(QStringLiteral("M2"));
        QCOMPARE(m2.size(), 1);
        const QStringList fields = m2[0].simplified().split(QLatin1Char(' '));
        QCOMPARE(fields.value(0), QStringLiteral("M2"));
        QCOMPARE(fields.value(1), QStringLiteral("broll_lo"));
        QCOMPARE(fields.value(2), QStringLiteral("012.5"));
        QCOMPARE(fields.value(3), QStringLiteral("00:00:40:00"));
        QVERIFY(others.contains(QStringLiteral("* FROM CLIP NAME: B-roll slow")));
    }

    void reverseSpeedHasANegativeM2()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("R"), {25, 1}, 1920, 1080, 2, 2);
        ClipData c;
        c.kind = ClipKind::Title;
        c.name = QStringLiteral("Title");
        c.start = 0;
        c.duration = 25;
        c.speed = -1.0;
        ops::overwriteClip(d, {TrackType::Video, 0}, c);
        QStringList others;
        const QVector<Line> lines = parse(edl::timelineToEdl(d, nullptr), &others);
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0].reel, QStringLiteral("AX"));
        QVERIFY(others.filter(QStringLiteral("M2")).value(0).contains(QStringLiteral("-25.0")));
    }

    void markersAsEdlAndCsv()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("M"), {25, 1}, 1920, 1080, 2, 2);
        Marker one;
        one.frame = 50;
        one.name = QStringLiteral("Intro, part 1");
        one.color = QColor(0x4c, 0x9b, 0xe8);
        one.note = QStringLiteral("say \"hi\"");
        Marker ranged;
        ranged.frame = 100;
        ranged.duration = 50;
        ranged.name = QStringLiteral("Scene");
        ranged.color = QColor(0xe0, 0x4a, 0x4a);
        d.markers = {one, ranged};

        QStringList others;
        const QVector<Line> lines = parse(edl::markersToEdl(d), &others);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[0].recIn, QStringLiteral("00:00:02:00"));
        QCOMPARE(lines[0].recOut, QStringLiteral("00:00:02:01"));
        QCOMPARE(lines[1].recIn, QStringLiteral("00:00:04:00"));
        QCOMPARE(lines[1].recOut, QStringLiteral("00:00:06:00"));
        QVERIFY(others.contains(QStringLiteral("* LOC: 00:00:02:00 BLUE     Intro, part 1")));
        QVERIFY(others.contains(QStringLiteral("* COMMENT: say \"hi\"")));
        QVERIFY(others.contains(QStringLiteral("* LOC: 00:00:04:00 RED      Scene")));

        const QStringList csv = edl::markersToCsv(d).split(QStringLiteral("\r\n"));
        QCOMPARE(csv[0], QStringLiteral("#,Name,Start,End,Duration,Color,Notes"));
        QCOMPARE(csv[1],
            QStringLiteral("1,\"Intro, part 1\",00:00:02:00,00:00:02:01,00:00:00:01,Blue,"
                           "\"say \"\"hi\"\"\""));
        QCOMPARE(csv[2], QStringLiteral("2,Scene,00:00:04:00,00:00:06:00,00:00:02:00,Red,"));
    }
};

QTEST_MAIN(TestEdlExport)
#include "test_edlexport.moc"
