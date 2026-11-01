// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for frame <-> timecode text conversion.
#include "ui/common/TimecodeFormat.h"

#include <QTest>

using namespace cw;

class TestTimecode : public QObject
{
    Q_OBJECT
private slots:
    void format_data()
    {
        QTest::addColumn<qint64>("frames");
        QTest::addColumn<int>("num");
        QTest::addColumn<int>("den");
        QTest::addColumn<QString>("text");
        QTest::newRow("zero") << qint64(0) << 25 << 1 << "00:00:00:00";
        QTest::newRow("one second") << qint64(25) << 25 << 1 << "00:00:01:00";
        QTest::newRow("hour") << qint64(25 * 3600 + 24) << 25 << 1 << "01:00:00:24";
        QTest::newRow("ntsc rounds to 30") << qint64(30 * 61 + 5) << 30000 << 1001 << "00:01:01:05";
        QTest::newRow("23.976") << qint64(24) << 24000 << 1001 << "00:00:01:00";
        QTest::newRow("negative") << qint64(-26) << 25 << 1 << "-00:00:01:01";
        QTest::newRow("100h") << qint64(25LL * 3600 * 100) << 25 << 1 << "100:00:00:00";
    }

    void format()
    {
        QFETCH(qint64, frames);
        QFETCH(int, num);
        QFETCH(int, den);
        QFETCH(QString, text);
        QCOMPARE(timecode::format(frames, Rational {num, den}), text);
        QCOMPARE(timecode::parse(text, Rational {num, den}), std::optional<int64_t>(frames));
    }

    void parseVariants()
    {
        const Rational fps {25, 1};
        QCOMPARE(timecode::parse(u"125", fps), std::optional<int64_t>(125));
        QCOMPARE(timecode::parse(u"01:00", fps), std::optional<int64_t>(25));
        QCOMPARE(timecode::parse(u"1:02:03", fps), std::optional<int64_t>(60 * 25 + 2 * 25 + 3));
        QCOMPARE(timecode::parse(u"00;00;01;05", fps), std::optional<int64_t>(30));
        QCOMPARE(timecode::parse(u" 00.00.02.00 ", fps), std::optional<int64_t>(50));
        QCOMPARE(timecode::parse(u"00:00:90:00", fps), std::optional<int64_t>(90 * 25));
        QCOMPARE(timecode::parse(u"", fps), std::nullopt);
        QCOMPARE(timecode::parse(u"ab:cd", fps), std::nullopt);
        QCOMPARE(timecode::parse(u"1:2:3:4:5", fps), std::nullopt);
        QCOMPARE(timecode::parse(u"-", fps), std::nullopt);
    }
};

QTEST_GUILESS_MAIN(TestTimecode)
#include "test_timecode.moc"
