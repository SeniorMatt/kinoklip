// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for smart bin rules (core/SmartBins.h): every field with every operator it offers,
// match all / any, ids, default smart bins.
#include "core/ClipColors.h"
#include "core/SmartBins.h"

#include <QDateTime>
#include <QTest>

using namespace kk;
using namespace kk::smartbins;

namespace {

/// 2026-03-15 12:00 local time.
const QDateTime kAdded(QDate(2026, 3, 15), QTime(12, 0));
const QDateTime kModified(QDate(2025, 12, 24), QTime(9, 30));
const QDateTime kNow(QDate(2026, 3, 20), QTime(12, 0));

MediaItem sample()
{
    MediaItem m;
    m.id = QStringLiteral("m");
    m.name = QStringLiteral("Beach Interview");
    m.path = QStringLiteral("/footage/day1/A001_C002.mov");
    m.info.valid = true;
    m.info.hasVideo = true;
    m.info.hasAudio = true;
    m.info.width = 3840;
    m.info.height = 2160;
    m.info.frameRate = {30000, 1001};
    m.info.durationUs = 12'500'000;
    m.info.videoCodec = QStringLiteral("prores");
    m.clipColor = QStringLiteral("teal");
    m.flags = flagsFromIds({QStringLiteral("red")});
    m.metadata.keywords = {QStringLiteral("Beach"), QStringLiteral("sunset")};
    m.metadata.comments = QStringLiteral("Great light, use this one");
    m.metadata.description = QStringLiteral("Wide shot of the bay");
    m.addedMs = kAdded.toMSecsSinceEpoch();
    m.fileModifiedMs = kModified.toMSecsSinceEpoch();
    return m;
}

struct Case
{
    SmartField field;
    SmartOp op;
    const char* value;
    bool expected;
};

} // namespace

class TestSmartBins : public QObject
{
    Q_OBJECT
private slots:
    void everyFieldAndOperator_data()
    {
        QTest::addColumn<int>("field");
        QTest::addColumn<int>("op");
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("expected");
        using F = SmartField;
        using O = SmartOp;
        // clang-format off
        const Case cases[] = {
            // Text: name (case-insensitive)
            {F::Name, O::Contains, "interview", true}, {F::Name, O::Contains, "city", false},
            {F::Name, O::NotContains, "city", true}, {F::Name, O::NotContains, "beach", false},
            {F::Name, O::Is, "beach interview", true}, {F::Name, O::Is, "beach", false},
            {F::Name, O::IsNot, "beach", true}, {F::Name, O::IsNot, "Beach Interview", false},
            {F::Name, O::StartsWith, "beach", true}, {F::Name, O::StartsWith, "interview", false},
            {F::Name, O::EndsWith, "VIEW", true}, {F::Name, O::EndsWith, "beach", false},
            // File name and path
            {F::FileName, O::Is, "a001_c002.mov", true}, {F::FileName, O::Contains, "day1", false},
            {F::FileName, O::NotContains, "C003", true}, {F::FileName, O::IsNot, "x.mov", true},
            {F::FileName, O::StartsWith, "A001", true}, {F::FileName, O::EndsWith, ".mp4", false},
            {F::Path, O::Contains, "/day1/", true}, {F::Path, O::StartsWith, "/footage", true},
            {F::Path, O::NotContains, "day1", false}, {F::Path, O::Is, "/footage", false},
            {F::Path, O::IsNot, "/footage", true}, {F::Path, O::EndsWith, ".mov", true},
            // Type
            {F::Type, O::Is, "video", true}, {F::Type, O::Is, "audio", false},
            {F::Type, O::IsNot, "still", true}, {F::Type, O::IsNot, "video", false},
            // Codec
            {F::Codec, O::Is, "ProRes", true}, {F::Codec, O::Contains, "h26", false},
            {F::Codec, O::NotContains, "h26", true}, {F::Codec, O::IsNot, "prores", false},
            {F::Codec, O::StartsWith, "pro", true}, {F::Codec, O::EndsWith, "res", true},
            // Resolution
            {F::Resolution, O::Is, "3840x2160", true}, {F::Resolution, O::Is, "1920 x 1080", false},
            {F::Resolution, O::IsNot, "1920x1080", true}, {F::Resolution, O::IsNot, "3840X2160", false},
            {F::Resolution, O::Greater, "1920x1080", true}, {F::Resolution, O::Greater, "4096x2160", false},
            {F::Resolution, O::Less, "4096x2160", true}, {F::Resolution, O::Less, "1920x1080", false},
            {F::Resolution, O::Is, "garbage", false},
            // Frame rate (29.97 matches 30000/1001)
            {F::FrameRate, O::Is, "29.97", true}, {F::FrameRate, O::Is, "30", false},
            {F::FrameRate, O::IsNot, "25", true}, {F::FrameRate, O::IsNot, "29,97", false},
            {F::FrameRate, O::Greater, "25", true}, {F::FrameRate, O::Greater, "29.97", false},
            {F::FrameRate, O::Less, "50", true}, {F::FrameRate, O::Less, "24", false},
            {F::FrameRate, O::Is, "abc", false},
            // Duration in seconds
            {F::Duration, O::Is, "12.5", true}, {F::Duration, O::Is, "12", false},
            {F::Duration, O::IsNot, "10", true}, {F::Duration, O::Greater, "10", true},
            {F::Duration, O::Greater, "20", false}, {F::Duration, O::Less, "20", true},
            {F::Duration, O::Less, "12.5", false},
            // Dates (now = 2026-03-20)
            {F::DateAdded, O::Is, "2026-03-15", true}, {F::DateAdded, O::Is, "2026-03-16", false},
            {F::DateAdded, O::Before, "2026-03-16", true}, {F::DateAdded, O::Before, "2026-03-15", false},
            {F::DateAdded, O::After, "2026-03-14", true}, {F::DateAdded, O::After, "2026-03-15", false},
            {F::DateAdded, O::InLast, "7", true}, {F::DateAdded, O::InLast, "3", false},
            {F::DateAdded, O::Is, "not a date", false},
            {F::DateModified, O::Is, "2025-12-24", true}, {F::DateModified, O::Before, "2026-01-01", true},
            {F::DateModified, O::After, "2026-01-01", false}, {F::DateModified, O::InLast, "30", false},
            {F::DateModified, O::InLast, "100", true},
            // Clip colour
            {F::ClipColor, O::Is, "teal", true}, {F::ClipColor, O::Is, "none", false},
            {F::ClipColor, O::IsNot, "blue", true}, {F::ClipColor, O::IsNot, "teal", false},
            // Flags
            {F::Flags, O::Has, "red", true}, {F::Flags, O::Has, "blue", false},
            {F::Flags, O::Has, "any", true}, {F::Flags, O::HasNot, "blue", true},
            {F::Flags, O::HasNot, "red", false}, {F::Flags, O::HasNot, "any", false},
            {F::Flags, O::Has, "bogus", false},
            // Keywords
            {F::Keywords, O::Has, "beach", true}, {F::Keywords, O::Has, "bea", false},
            {F::Keywords, O::HasNot, "city", true}, {F::Keywords, O::HasNot, "SUNSET", false},
            {F::Keywords, O::Contains, "suns", true}, {F::Keywords, O::Contains, "city", false},
            {F::Keywords, O::NotContains, "city", true}, {F::Keywords, O::NotContains, "ach", false},
            // Comments / description
            {F::Comments, O::Contains, "great", true}, {F::Comments, O::NotContains, "great", false},
            {F::Comments, O::Is, "x", false}, {F::Comments, O::IsNot, "x", true},
            {F::Comments, O::StartsWith, "great light", true}, {F::Comments, O::EndsWith, "one", true},
            {F::Description, O::Contains, "bay", true}, {F::Description, O::StartsWith, "bay", false},
            {F::Description, O::NotContains, "bay", false}, {F::Description, O::Is, "wide shot of the bay", true},
            {F::Description, O::IsNot, "x", true}, {F::Description, O::EndsWith, "bay", true},
            // Usage (the item has one use in the context below)
            {F::Usage, O::Is, "used", true}, {F::Usage, O::Is, "unused", false},
            {F::Usage, O::IsNot, "unused", true}, {F::Usage, O::IsNot, "used", false},
            // Operators a field does not offer never match
            {F::Name, O::Greater, "a", false}, {F::Type, O::Contains, "vid", false},
            {F::Flags, O::Is, "red", false},
        };
        // clang-format on
        for (const Case& c : cases) {
            const QString tag = fieldId(c.field) + QLatin1Char(' ') + operatorId(c.op)
                + QLatin1Char(' ') + QLatin1String(c.value);
            QTest::newRow(qPrintable(tag))
                << int(c.field) << int(c.op) << QString::fromUtf8(c.value) << c.expected;
        }
    }

    void everyFieldAndOperator()
    {
        QFETCH(int, field);
        QFETCH(int, op);
        QFETCH(QString, value);
        QFETCH(bool, expected);
        const QHash<QString, int> usage {{QStringLiteral("m"), 1}};
        const Context ctx {&usage, kNow.toMSecsSinceEpoch()};
        QCOMPARE(ruleMatches(SmartRule {SmartField(field), SmartOp(op), value}, sample(), ctx),
            expected);
    }

    void everyOfferedOperatorIsCovered()
    {
        // Every field offers operators, and each one has a stable id that parses back.
        for (const SmartField f : fields()) {
            QVERIFY(!operatorsFor(f).isEmpty());
            SmartField parsed {};
            QVERIFY(fieldFromId(fieldId(f), &parsed));
            QCOMPARE(parsed, f);
            QVERIFY(!fieldLabel(f).isEmpty());
            for (const SmartOp o : operatorsFor(f)) {
                SmartOp p {};
                QVERIFY(operatorFromId(operatorId(o), &p));
                QCOMPARE(p, o);
                QVERIFY(!operatorLabel(o).isEmpty());
            }
            if (valueKind(f) == ValueKind::Choice || valueKind(f) == ValueKind::Flags)
                QVERIFY(!choicesFor(f).isEmpty());
        }
        SmartField f {};
        QVERIFY(!fieldFromId(QStringLiteral("mood"), &f));
        SmartOp o {};
        QVERIFY(!operatorFromId(QStringLiteral("resembles"), &o));
    }

    void unknownValuesAndKinds()
    {
        const Context ctx {nullptr, kNow.toMSecsSinceEpoch()};
        MediaItem m = sample();
        m.addedMs = 0; // unknown date: no comparison matches
        QVERIFY(!ruleMatches(
            {SmartField::DateAdded, SmartOp::Before, QStringLiteral("2030-01-01")}, m, ctx));
        QVERIFY(
            !ruleMatches({SmartField::DateAdded, SmartOp::InLast, QStringLiteral("9999")}, m, ctx));
        // No usage table = unused.
        QVERIFY(ruleMatches({SmartField::Usage, SmartOp::Is, QStringLiteral("unused")}, m, ctx));
        m.clipColor.clear();
        QVERIFY(ruleMatches({SmartField::ClipColor, SmartOp::Is, QStringLiteral("none")}, m, ctx));

        MediaItem still;
        still.info.hasVideo = true;
        still.info.isStillImage = true;
        QCOMPARE(itemType(still), QStringLiteral("still"));
        MediaItem audio;
        audio.info.hasAudio = true;
        QCOMPARE(itemType(audio), QStringLiteral("audio"));
        QVERIFY(ruleMatches(
            {SmartField::Resolution, SmartOp::IsNot, QStringLiteral("1x1")}, audio, ctx));
        QVERIFY(!ruleMatches({SmartField::Resolution, SmartOp::Less, QStringLiteral("100x100")},
            audio, ctx)); // no picture: neither larger nor smaller
        MediaItem timeline;
        timeline.kind = MediaItemKind::Timeline;
        timeline.path = QStringLiteral("/never");
        QCOMPARE(itemType(timeline), QStringLiteral("timeline"));
        QVERIFY(!ruleMatches(
            {SmartField::Path, SmartOp::Contains, QStringLiteral("never")}, timeline, ctx));
        MediaItem compound;
        compound.kind = MediaItemKind::CompoundClip;
        QCOMPARE(itemType(compound), QStringLiteral("compound"));
        MediaItem subtitle;
        subtitle.kind = MediaItemKind::Subtitle;
        QCOMPARE(itemType(subtitle), QStringLiteral("subtitle"));
        QVERIFY(ruleMatches(
            {SmartField::Type, SmartOp::Is, QStringLiteral("subtitle")}, subtitle, ctx));
    }

    void matchAllAndAny()
    {
        const Context ctx {nullptr, kNow.toMSecsSinceEpoch()};
        const SmartRule yes {SmartField::Type, SmartOp::Is, QStringLiteral("video")};
        const SmartRule no {SmartField::Codec, SmartOp::Is, QStringLiteral("h264")};
        SmartBin bin;
        bin.rules = {yes, no};
        bin.matchAll = true;
        QVERIFY(!matches(bin, sample(), ctx));
        bin.matchAll = false;
        QVERIFY(matches(bin, sample(), ctx));
        bin.rules = {no};
        QVERIFY(!matches(bin, sample(), ctx));
        bin.rules = {yes};
        bin.matchAll = true;
        QVERIFY(matches(bin, sample(), ctx));
        bin.rules.clear(); // no rules: shows nothing
        QVERIFY(!matches(bin, sample(), ctx));
        bin.matchAll = false;
        QVERIFY(!matches(bin, sample(), ctx));
    }

    void defaultSmartBinsSortByType()
    {
        const QVector<SmartBin> defaults = defaultSmartBins();
        QCOMPARE(defaults.size(), 4);
        for (const SmartBin& b : defaults)
            QVERIFY(b.id.startsWith(QLatin1String(kDefaultIdPrefix)));
        const Context ctx;
        QVERIFY(matches(defaults[0], sample(), ctx)); // Video
        QVERIFY(!matches(defaults[1], sample(), ctx)); // Audio
        MediaItem timeline;
        timeline.kind = MediaItemKind::Timeline;
        QVERIFY(matches(defaults[3], timeline, ctx));
    }
};

QTEST_GUILESS_MAIN(TestSmartBins)
#include "test_smartbins.moc"
