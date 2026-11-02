// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for retiming: curve maths (segments, ramps, reverse, freeze), media limits,
// RetimeOps (ripple, partners), trims / splits / joins of retimed clips, serialization.
#include "CoreTestUtils.h"
#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/RetimeOps.h"
#include "core/Serialization.h"
#include "core/Timeline.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;
namespace rops = kk::retimeops;

namespace {

/// Source frame (25 fps source on a 25 fps timeline) shown at timeline frame @p frame.
int64_t srcFrame(const ClipData& c, int64_t frame)
{
    return c.sourceTimeUsAt(frame, Rational {25, 1}) / kUsPerFrame25;
}

/// A linked V+A pair on V1 / A1.
std::pair<quint64, quint64> placePair(TimelineData& d, ClipData c)
{
    c.linkGroup = d.allocateId();
    ClipData a = c;
    const quint64 v = ops::overwriteClip(d, kV1, c);
    const quint64 au = ops::overwriteClip(d, kA1, a);
    return {v, au};
}

} // namespace

class TestRetime : public QObject
{
    Q_OBJECT
private slots:
    // ---- Mapping maths ----
    void constantSpeedMatchesPlanFormula()
    {
        ClipData c = mediaClip(10, 50, 5, 1000);
        c.speed = 2.0;
        QVERIFY(!retime::hasCurve(c));
        QVERIFY(retime::isRetimed(c));
        QCOMPARE(srcFrame(c, 10), 5);
        QCOMPARE(srcFrame(c, 20), 25);
        QCOMPARE(c.sourceTimeUsAt(13, Rational {25, 1}), 5 * kUsPerFrame25 + 6 * kUsPerFrame25);
    }

    void reverseAndFreezeConstant()
    {
        ClipData c = mediaClip(0, 10, 100, 1000);
        c.speed = -1.0;
        QCOMPARE(srcFrame(c, 0), 100);
        QCOMPARE(srcFrame(c, 9), 91);
        c.speed = 0.0;
        QCOMPARE(srcFrame(c, 0), 100);
        QCOMPARE(srcFrame(c, 9), 100);
        QCOMPARE(retime::segments(c).size(), 1);
        QVERIFY(retime::segments(c).front().isFreeze());
    }

    void curveSegmentsAndExtrapolation()
    {
        // 0..10 at 100 %, 10..30 at 50 %, 30..40 at 200 %.
        ClipData c = mediaClip(0, 40, 0, 1000);
        AnimatedValue& v = c.retime.animated(keys::RetimeFrame, 0.0);
        v.setKey(0, 0.0);
        v.setKey(10, 10.0);
        v.setKey(30, 20.0);
        v.setKey(40, 40.0);
        QVERIFY(retime::hasCurve(c));
        QCOMPARE(srcFrame(c, 5), 5);
        QCOMPARE(srcFrame(c, 20), 15);
        QCOMPARE(srcFrame(c, 35), 30);
        // Past the last key: the last segment's speed continues (200 %).
        QCOMPARE(srcFrame(c, 42), 44);
        QCOMPARE(srcFrame(c, -2), -2);
        const QVector<retime::Segment> s = retime::segments(c);
        QCOMPARE(s.size(), 3);
        QCOMPARE(s[0].speed, 1.0);
        QCOMPARE(s[1].speed, 0.5);
        QCOMPARE(s[2].speed, 2.0);
        QCOMPARE(s[1].startLocal, 10);
        QCOMPARE(s[1].endLocal, 30);
        QCOMPARE(retime::speedPoints(c), (QVector<int64_t> {10, 30}));
        QCOMPARE(retime::segmentAt(s, 29), 1);
        QCOMPARE(retime::formatPercent(0.5), QStringLiteral("50%"));
        QCOMPARE(retime::formatPercent(1.0 / 3.0), QStringLiteral("33.3%"));
        QCOMPARE(retime::formatPercent(-1.0), QStringLiteral("-100%"));
    }

    void smoothRampIsMonotoneAndContinuous()
    {
        ClipData c = mediaClip(0, 60, 0, 1000);
        AnimatedValue& v = c.retime.animated(keys::RetimeFrame, 0.0);
        v.setKey(0, 0.0);
        v.setKey(30, 30.0); // 100 %
        v.setKey(60, 45.0); // 50 %
        ClipData ramp = c;
        rops::ensureCurve(ramp);
        Keyframe& k = ramp.retime.values[keys::RetimeFrame].keys[1];
        k.in = KeyHandle::withMode(KeyHandle::Mode::Smooth);
        k.out = KeyHandle::withMode(KeyHandle::Mode::Smooth);
        // Speed changes gradually around the point instead of jumping.
        double previous = retime::position(ramp, 0.0);
        double lastSpeed = retime::speedAt(ramp, 1.0);
        for (double x = 0.5; x <= 60.0; x += 0.5) {
            const double p = retime::position(ramp, x);
            QVERIFY(p >= previous);
            const double s = retime::speedAt(ramp, x);
            QVERIFY2(std::abs(s - lastSpeed) < 0.05, qPrintable(QString::number(x)));
            lastSpeed = s;
            previous = p;
        }
        QVERIFY(std::abs(retime::speedAt(ramp, 30.0) - 0.75) < 0.05);
        // The ramp still lands on the speed point's source frame.
        QCOMPARE(retime::position(ramp, 30.0), 30.0);
    }

    void mediaLimitsFollowTheCurve()
    {
        // Reverse clip showing source 49..40 of a 50-frame source: the head can extend until
        // the last source frame (49 -> 49 + 0 more), the tail until source 0.
        ClipData c = mediaClip(0, 10, 49, 50);
        c.speed = -1.0;
        QCOMPARE(ops::maxHeadExtension(c, Rational {25, 1}), 0);
        QCOMPARE(ops::maxTailExtension(c, Rational {25, 1}), 40);
        // Freeze: unbounded both ways.
        c.speed = 0.0;
        QVERIFY(ops::maxTailExtension(c, Rational {25, 1}) > 1'000'000);
        // Curve at 200 % after frame 10 on a 100-frame source starting at 0.
        ClipData k = mediaClip(0, 20, 0, 100);
        AnimatedValue& v = k.retime.animated(keys::RetimeFrame, 0.0);
        v.setKey(0, 0.0);
        v.setKey(10, 10.0);
        v.setKey(20, 30.0);
        // Source used up to frame 30; 70 more source frames at 200 % = 35 timeline frames.
        QCOMPARE(ops::maxTailExtension(k, Rational {25, 1}), 35);
        QCOMPARE(ops::maxHeadExtension(k, Rational {25, 1}), 0);
    }

    // ---- Ops ----
    void speedPointThenSlowSegmentRipples()
    {
        TimelineData d = timeline();
        const quint64 id = ops::overwriteClip(d, kV1, mediaClip(0, 40, 0, 1000));
        const quint64 next = ops::overwriteClip(d, kV1, mediaClip(40, 10, 0, 1000));
        QVERIFY(rops::addSpeedPoint(d, id, 20));
        QCOMPARE(retime::speedPoints(*ops::clipById(d, id)), (QVector<int64_t> {20}));
        QVERIFY(!rops::addSpeedPoint(d, id, 20)); // already there
        // Second segment (20..40) at 50 %: 20 frames of source now last 40.
        QVERIFY(rops::setSegmentSpeed(d, id, 1, 0.5));
        const ClipData& c = *ops::clipById(d, id);
        QCOMPARE(c.duration, 60);
        QCOMPARE(ops::clipById(d, next)->start, 60);
        QCOMPARE(srcFrame(c, 19), 19);
        QCOMPARE(srcFrame(c, 40), 30);
        QCOMPARE(srcFrame(c, 59), 39);
        KK_VERIFY_INVARIANTS(d);
        // Back to 100 %: the same layout as before and a plain clip again.
        QVERIFY(rops::setSegmentSpeed(d, id, 1, 1.0));
        QCOMPARE(layout(d, kV1), QStringLiteral("0:40 40:10"));
        QCOMPARE(retime::speedPoints(*ops::clipById(d, id)), (QVector<int64_t> {20})); // kept
        rops::addSpeedPoint(d, id, 30);
        QVERIFY(rops::removeSpeedPoint(d, id, 20));
        QCOMPARE(retime::speedPoints(*ops::clipById(d, id)), (QVector<int64_t> {30}));
        QVERIFY(rops::removeSpeedPoint(d, id, 30));
        QVERIFY(!retime::hasCurve(*ops::clipById(d, id)));
        QCOMPARE(ops::clipById(d, id)->speed, 1.0);
    }

    void linkedAudioFollows()
    {
        TimelineData d = timeline();
        const auto [v, a] = placePair(d, mediaClip(0, 30, 0, 1000));
        QCOMPARE(rops::partners(d, v), (QVector<quint64> {v, a}));
        QCOMPARE(rops::primaries(d, {v, a}), (QVector<quint64> {v}));
        QVERIFY(rops::addSpeedPoint(d, v, 10));
        QVERIFY(rops::setSegmentSpeed(d, v, 0, 2.0));
        const ClipData& cv = *ops::clipById(d, v);
        const ClipData& ca = *ops::clipById(d, a);
        QCOMPARE(cv.duration, 25);
        QCOMPARE(ca.duration, 25);
        QCOMPARE(cv.retime, ca.retime);
        KK_VERIFY_INVARIANTS(d);
    }

    void reverseSegmentBounces()
    {
        TimelineData d = timeline();
        const quint64 id = ops::overwriteClip(d, kV1, mediaClip(0, 30, 100, 1000));
        rops::addSpeedPoint(d, id, 10);
        rops::addSpeedPoint(d, id, 20);
        QVERIFY(rops::reverseSegment(d, id, 1));
        const ClipData& c = *ops::clipById(d, id);
        QCOMPARE(c.duration, 30);
        QCOMPARE(srcFrame(c, 10), 110);
        QCOMPARE(srcFrame(c, 15), 105);
        QCOMPARE(srcFrame(c, 20), 100); // played back to where it was at frame 0
        QCOMPARE(srcFrame(c, 25), 105); // and forward again
        QCOMPARE(retime::segments(c)[1].speed, -1.0);
        QVERIFY(!rops::reverseSegment(d, id, 5));
    }

    void freezeSegmentAndFreezeFrame()
    {
        TimelineData d = timeline();
        const quint64 id = ops::overwriteClip(d, kV1, mediaClip(0, 20, 0, 1000));
        const quint64 next = ops::overwriteClip(d, kV1, mediaClip(20, 5));
        QVERIFY(rops::insertFreeze(d, id, 8, 12));
        const ClipData& c = *ops::clipById(d, id);
        QCOMPARE(c.duration, 32);
        QCOMPARE(ops::clipById(d, next)->start, 32);
        for (int64_t f = 8; f <= 20; ++f)
            QCOMPARE(srcFrame(c, f), 8);
        QCOMPARE(srcFrame(c, 21), 9);
        QCOMPARE(srcFrame(c, 31), 19);
        const QVector<retime::Segment> s = retime::segments(c);
        QCOMPARE(s.size(), 3);
        QVERIFY(s[1].isFreeze());
        // Shift+R: whole clip on the playhead frame, same length.
        QVERIFY(rops::freezeClip(d, id, 25));
        const ClipData& f = *ops::clipById(d, id);
        QCOMPARE(f.duration, 32);
        QCOMPARE(f.speed, 0.0);
        QVERIFY(!retime::hasCurve(f));
        QCOMPARE(srcFrame(f, 0), 13); // frame 25 showed source 13 (after the freeze)
        QCOMPARE(srcFrame(f, 31), 13);
        KK_VERIFY_INVARIANTS(d);
        QVERIFY(rops::resetClip(d, id));
        QCOMPARE(ops::clipById(d, id)->speed, 1.0);
        QCOMPARE(ops::clipById(d, id)->duration, 32);
    }

    void retimeAndSlideHandles()
    {
        TimelineData d = timeline();
        const quint64 id = ops::overwriteClip(d, kV1, mediaClip(0, 40, 0, 1000));
        rops::addSpeedPoint(d, id, 20);
        // Bottom handle: segment 0 (20 frames of source) stretched to 40 frames = 50 %.
        QCOMPARE(rops::retimePoint(d, id, 20, 20), 20);
        ClipData c = *ops::clipById(d, id);
        QCOMPARE(c.duration, 60);
        QCOMPARE(retime::segments(c)[0].speed, 0.5);
        QCOMPARE(retime::segments(c)[1].speed, 1.0);
        QCOMPARE(srcFrame(c, 40), 20);
        // The clip end is a retime handle too.
        QCOMPARE(rops::retimePoint(d, id, 60, -10), -10);
        c = *ops::clipById(d, id);
        QCOMPARE(c.duration, 50);
        QCOMPARE(retime::segments(c)[1].speed, 2.0);
        // Never shorter than one frame.
        QCOMPARE(rops::retimePoint(d, id, 50, -100), -9);
        // Top handle: point at 40 moves 10 later; 50 % segment covers 5 more source frames,
        // the 200 % segment (now 11 source... frames) keeps its speed.
        TimelineData e = timeline();
        const quint64 id2 = ops::overwriteClip(e, kV1, mediaClip(0, 40, 0, 1000));
        rops::addSpeedPoint(e, id2, 20);
        rops::setSegmentSpeed(e, id2, 0, 0.5); // 0..40 at 50 % (20 src), 40..60 at 100 %
        QCOMPARE(rops::slidePoint(e, id2, 40, 10), 10);
        const ClipData& s = *ops::clipById(e, id2);
        const QVector<retime::Segment> segs = retime::segments(s);
        QCOMPARE(segs[0].speed, 0.5);
        QCOMPARE(segs[1].speed, 1.0);
        QCOMPARE(segs[0].length(), 50);
        QCOMPARE(segs[1].length(), 15); // source 25..40
        QCOMPARE(s.duration, 65);
        QCOMPARE(srcFrame(s, 64), 39);
    }

    void rampPresetOnPoint()
    {
        TimelineData d = timeline();
        const quint64 id = ops::overwriteClip(d, kV1, mediaClip(0, 40, 0, 1000));
        rops::addSpeedPoint(d, id, 20);
        rops::setSegmentSpeed(d, id, 1, 0.5);
        QVERIFY(rops::setPointRamp(d, id, 20, Interp::Smooth));
        const QVector<retime::Segment> s = retime::segments(*ops::clipById(d, id));
        QVERIFY(s[0].rampOut);
        QVERIFY(s[1].rampIn);
        QVERIFY(!rops::setPointRamp(d, id, 20, Interp::Smooth));
        QVERIFY(rops::setPointRamp(d, id, 20, Interp::Linear));
    }

    void changeClipSpeedDialog()
    {
        TimelineData d = timeline();
        const quint64 id = ops::overwriteClip(d, kV1, mediaClip(0, 40, 10, 100));
        const quint64 next = ops::overwriteClip(d, kV1, mediaClip(40, 10));
        rops::SpeedChange ch;
        ch.speed = 0.5;
        QVERIFY(rops::changeClipSpeed(d, id, ch));
        QCOMPARE(ops::clipById(d, id)->duration, 80);
        QCOMPARE(ops::clipById(d, next)->start, 80);
        // Reverse over the same source (10..49): starts on 49.
        ch.speed = 1.0;
        ch.reverse = true;
        QVERIFY(rops::changeClipSpeed(d, id, ch));
        const ClipData& r = *ops::clipById(d, id);
        QCOMPARE(r.duration, 40);
        QCOMPARE(srcFrame(r, 0), 49);
        QCOMPARE(srcFrame(r, 39), 10);
        // No ripple: the length stays, limited by the media (90 frames left at 200 % = 45).
        ch.reverse = false;
        ch.ripple = false;
        ch.speed = 4.0;
        QVERIFY(rops::changeClipSpeed(d, id, ch));
        QCOMPARE(ops::clipById(d, id)->duration, 22);
        QCOMPARE(ops::clipById(d, next)->start, 40);
        ch.freeze = true;
        ch.freezeFrame = 5;
        QVERIFY(rops::changeClipSpeed(d, id, ch));
        QCOMPARE(ops::clipById(d, id)->speed, 0.0);
        QCOMPARE(srcFrame(*ops::clipById(d, id), 0), 30);
        KK_VERIFY_INVARIANTS(d);
    }

    void trimSplitJoinKeepTheMapping()
    {
        TimelineData d = timeline();
        const quint64 id = ops::overwriteClip(d, kV1, mediaClip(0, 40, 0, 1000));
        rops::addSpeedPoint(d, id, 10);
        rops::setSegmentSpeed(d, id, 1, 0.5); // 10..70 at 50 %
        const ClipData before = *ops::clipById(d, id);
        QCOMPARE(before.duration, 70);
        // Head trim: every remaining frame shows the same source.
        QCOMPARE(ops::trimHead(d, id, 15, false), 15);
        const ClipData& trimmed = *ops::clipById(d, id);
        for (int64_t f = 15; f < 70; ++f)
            QCOMPARE(srcFrame(trimmed, f), srcFrame(before, f));
        QCOMPARE(ops::trimHead(d, id, -15, false), -15);
        QCOMPARE(*ops::clipById(d, id), before);
        // Split + join round trip.
        const QVector<quint64> right = ops::splitClips(d, {id}, 30);
        QCOMPARE(right.size(), 1);
        for (int64_t f = 30; f < 70; ++f)
            QCOMPARE(srcFrame(*ops::clipById(d, right[0]), f), srcFrame(before, f));
        QVERIFY(ops::canJoin(d, id, right[0]));
        QVERIFY(ops::joinClips(d, id, right[0]));
        const ClipData joined = *ops::clipById(d, id);
        for (int64_t f = 0; f < 70; ++f)
            QCOMPARE(srcFrame(joined, f), srcFrame(before, f));
        // Slip moves every shown frame by the same amount.
        QVERIFY(ops::slipClip(d, id, 5) == 5);
        for (int64_t f = 0; f < 70; f += 7)
            QCOMPARE(srcFrame(*ops::clipById(d, id), f), srcFrame(before, f) + 5);
        KK_VERIFY_INVARIANTS(d);
    }

    void undoIsOneStepPerOperation()
    {
        Project p;
        p.beginEdit(QStringLiteral("Place"));
        const quint64 id
            = ops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 40, 0, 1000));
        p.endEdit();
        const TimelineData placed = p.timeline()->data();
        const int steps = p.undoStack()->count();
        p.beginEdit(QStringLiteral("Add Speed Point"));
        rops::addSpeedPoint(p.timeline()->mutableData(), id, 10);
        p.endEdit();
        p.beginEdit(QStringLiteral("Change Speed"));
        rops::setSegmentSpeed(p.timeline()->mutableData(), id, 0, 0.25);
        p.endEdit();
        QCOMPARE(p.undoStack()->count(), steps + 2);
        QCOMPARE(ops::clipById(p.timeline()->data(), id)->duration, 70);
        p.undo();
        QCOMPARE(ops::clipById(p.timeline()->data(), id)->duration, 40);
        p.undo();
        QCOMPARE(p.timeline()->data(), placed);
    }

    // ---- Serialization ----
    void serializationRoundTrip()
    {
        TimelineData d = timeline();
        d.retimeProcess = RetimeProcess::FrameBlend;
        const quint64 id = ops::overwriteClip(d, kV1, mediaClip(0, 40, 0, 1000));
        rops::addSpeedPoint(d, id, 10);
        rops::setSegmentSpeed(d, id, 1, 0.5);
        rops::setPointRamp(d, id, 10, Interp::Smooth);
        rops::setProcess(d, id, RetimeProcess::OpticalFlow);
        rops::setPitchCorrection(d, id, false);
        const QJsonObject json = serialization::timelineToJson(d);
        TimelineData back;
        QString error;
        QVERIFY2(serialization::timelineFromJson(json, back, &error), qPrintable(error));
        QCOMPARE(back, d);
        const QJsonObject clip = json.value(QLatin1String("videoTracks"))
                                     .toArray()[0]
                                     .toObject()
                                     .value(QLatin1String("clips"))
                                     .toArray()[0]
                                     .toObject();
        QVERIFY(!clip.contains(QLatin1String("speed")));
        QCOMPARE(clip.value(QLatin1String("retime")).toObject().value(QLatin1String("process")),
            QJsonValue(QStringLiteral("opticalFlow")));
    }

    void formatThreeSpeedMigrates()
    {
        // A version-3 clip: top-level "speed", no "retime" object.
        ClipData c = mediaClip(0, 20, 0, 1000);
        c.id = 1;
        QJsonObject o = serialization::clipToJson(c);
        o.remove(QLatin1String("retime"));
        o[QLatin1String("speed")] = 1.5;
        const ClipData back = serialization::clipFromJson(o);
        QCOMPARE(back.speed, 1.5);
        QVERIFY(!retime::hasCurve(back));
        QVERIFY(!back.pitchCorrection); // old projects resampled their audio
        QCOMPARE(retime::segments(back).size(), 1);
        o[QLatin1String("speed")] = 1.0;
        QVERIFY(serialization::clipFromJson(o).pitchCorrection);
        QCOMPARE(serialization::kFormatVersion, 5);
    }

    void projectDefaultProcessFollowsSettings()
    {
        Project p;
        ProjectSettings s = p.settings();
        s.retimeProcess = RetimeProcess::OpticalFlow;
        p.setSettings(s);
        QCOMPARE(p.timeline()->data().retimeProcess, RetimeProcess::OpticalFlow);
        ClipData c = mediaClip(0, 10);
        c.speed = 0.5;
        QCOMPARE(retime::effectiveProcess(c, p.timeline()->data()), RetimeProcess::OpticalFlow);
        c.retimeProcess = RetimeProcess::FrameBlend;
        QCOMPARE(retime::effectiveProcess(c, p.timeline()->data()), RetimeProcess::FrameBlend);
        c.speed = 1.0;
        QCOMPARE(retime::effectiveProcess(c, p.timeline()->data()), RetimeProcess::Nearest);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("p.kkproj"));
        QVERIFY(p.save(path));
        Project q;
        QVERIFY(q.load(path));
        QCOMPARE(q.settings().retimeProcess, RetimeProcess::OpticalFlow);
    }
};

QTEST_GUILESS_MAIN(TestRetime)
#include "test_retime.moc"
