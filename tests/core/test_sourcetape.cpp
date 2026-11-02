// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the Cut page source tape (core/SourceTape.h): layout of a bin's clips back to
// back and the mapping of tape frames and ranges across clip boundaries (spec 35.4, 35.5).
#include "core/SourceTape.h"

#include <QTest>

using namespace kk;

namespace {

MediaItem item(const QString& id, int64_t durationUs, bool video = true, bool audio = true)
{
    MediaItem m;
    m.id = id;
    m.name = id;
    m.path = QStringLiteral("/media/") + id + QStringLiteral(".mp4");
    m.info.valid = true;
    m.info.hasVideo = video;
    m.info.hasAudio = audio;
    m.info.durationUs = durationUs;
    m.info.frameRate = Rational {25, 1};
    return m;
}

} // namespace

class TestSourceTape : public QObject
{
    Q_OBJECT
private slots:
    void clipsBackToBackInOrder()
    {
        const MediaItem a = item(QStringLiteral("a"), 2'000'000); // 50 frames
        const MediaItem b = item(QStringLiteral("b"), 1'010'000); // 25.25 -> 26 frames
        MediaItem still = item(QStringLiteral("s"), 0, true, false);
        still.info.isStillImage = true;
        MediaItem timeline = item(QStringLiteral("t"), 4'000'000);
        timeline.kind = MediaItemKind::Timeline; // not on the tape
        const SourceTape tape
            = SourceTape::build({&a, &timeline, &b, &still}, Rational {25, 1}, 125);
        QCOMPARE(int(tape.entries().size()), 3);
        QCOMPARE(tape.entries()[0].start, int64_t(0));
        QCOMPARE(tape.entries()[0].duration, int64_t(50));
        QCOMPARE(tape.entries()[1].start, int64_t(50));
        QCOMPARE(tape.entries()[1].duration, int64_t(26));
        QCOMPARE(tape.entries()[2].start, int64_t(76));
        QCOMPARE(tape.entries()[2].duration, int64_t(125));
        QCOMPARE(tape.duration(), int64_t(201));
        QCOMPARE(tape.boundaries(), (std::vector<int64_t> {0, 50, 76, 201}));
        QCOMPARE(tape.indexOf(QStringLiteral("b")), 1);
        QCOMPARE(tape.indexOf(QStringLiteral("t")), -1);
    }

    void locateAcrossBoundaries()
    {
        const MediaItem a = item(QStringLiteral("a"), 2'000'000);
        const MediaItem b = item(QStringLiteral("b"), 2'000'000);
        const SourceTape tape = SourceTape::build({&a, &b}, Rational {25, 1}, 125);
        QCOMPARE(tape.entryAt(-1), -1);
        QCOMPARE(tape.entryAt(0), 0);
        QCOMPARE(tape.entryAt(49), 0);
        QCOMPARE(tape.entryAt(50), 1); // first frame of b
        QCOMPARE(tape.entryAt(99), 1);
        QCOMPARE(tape.entryAt(100), -1); // past the end
        const SourceTapePosition p = tape.locate(60);
        QCOMPARE(p.entry, 1);
        QCOMPARE(p.sourceUs, int64_t(400'000)); // 10 frames into b
        QCOMPARE(tape.frameOf(1, 400'000), int64_t(60));
        QCOMPARE(tape.frameOf(1, 99'000'000), int64_t(99)); // clamped into the clip
        QCOMPARE(tape.frameOf(5, 0), int64_t(-1));
    }

    void rangeSplitsIntoSegments()
    {
        const MediaItem a = item(QStringLiteral("a"), 2'000'000); // [0, 50)
        const MediaItem b = item(QStringLiteral("b"), 1'000'000); // [50, 75)
        const MediaItem c = item(QStringLiteral("c"), 2'000'000); // [75, 125)
        const SourceTape tape = SourceTape::build({&a, &b, &c}, Rational {25, 1}, 125);
        // In inside a, out inside c: the tail of a, all of b, the head of c.
        const QVector<SourceTapeSegment> s = tape.segments(40, 85);
        QCOMPARE(s.size(), 3);
        QCOMPARE(s[0], (SourceTapeSegment {QStringLiteral("a"), 1'600'000, 2'000'000}));
        QCOMPARE(s[1], (SourceTapeSegment {QStringLiteral("b"), 0, 1'000'000}));
        QCOMPARE(s[2], (SourceTapeSegment {QStringLiteral("c"), 0, 400'000}));
        // Inside one clip.
        const QVector<SourceTapeSegment> one = tape.segments(55, 60);
        QCOMPARE(one.size(), 1);
        QCOMPARE(one[0], (SourceTapeSegment {QStringLiteral("b"), 200'000, 400'000}));
        // Exactly one clip, and clamping to the tape.
        QCOMPARE(tape.segments(50, 75).size(), 1);
        QCOMPARE(tape.segments(-10, 500).size(), 3);
        QVERIFY(tape.segments(60, 60).isEmpty());
    }

    void timelineIsPlayable()
    {
        const MediaItem a = item(QStringLiteral("a"), 2'000'000);
        const MediaItem b = item(QStringLiteral("b"), 1'000'000, true, false);
        const SourceTape tape = SourceTape::build({&a, &b}, Rational {25, 1}, 125);
        const TimelineData tl = tape.toTimeline(QStringLiteral("Tape"), 1920, 1080);
        QCOMPARE(tl.duration(), int64_t(75));
        QCOMPARE(int(tl.videoTracks[0].clips.size()), 2);
        QCOMPARE(int(tl.audioTracks[0].clips.size()), 1); // b has no sound
        QCOMPARE(tl.videoTracks[0].clips[1].start, int64_t(50));
        QCOMPARE(tl.videoTracks[0].clips[1].mediaId, QStringLiteral("b"));
    }
};

QTEST_GUILESS_MAIN(TestSourceTape)
#include "test_sourcetape.moc"
