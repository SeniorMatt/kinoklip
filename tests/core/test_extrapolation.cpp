// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for keyframe extrapolation (loops), Reverse, Time Stretch and Step In / Out
// (docs/behavior-spec.md section 43).
#include "CoreTestUtils.h"
#include "core/Composition.h"
#include "core/KeyframeOps.h"
#include "core/ParamKeys.h"
#include "core/Serialization.h"

#include <QJsonDocument>
#include <QTest>

using namespace kk;
using namespace kk::test;
namespace ko = kk::keyframeops;

namespace {
// 10 -> 30 over frames 10..20 (linear), a ramp with a net rise of 20.
AnimatedValue ramp()
{
    AnimatedValue v(0.0);
    v.setKey(10, 10.0, Interp::Linear);
    v.setKey(20, 30.0, Interp::Linear);
    return v;
}
} // namespace

class TestExtrapolation : public QObject
{
    Q_OBJECT
private slots:
    void spec43_1_constantByDefault()
    {
        const AnimatedValue v = ramp();
        QCOMPARE(v.pre, Extrapolation::Constant);
        QCOMPARE(v.post, Extrapolation::Constant);
        QCOMPARE(v.valueAt(0), 10.0);
        QCOMPARE(v.valueAt(35), 30.0);
        QVERIFY(!v.isExtrapolated());
    }

    void spec43_2_loopBeforeAndAfter()
    {
        AnimatedValue v = ramp();
        v.post = Extrapolation::Loop;
        QCOMPARE(v.valueAt(20), 30.0); // the last key itself
        QCOMPARE(v.valueAt(25), 20.0); // halfway through the 2nd repeat
        QCOMPARE(v.valueAt(30), 10.0);
        QCOMPARE(v.valueAt(0), 10.0); // pre still constant
        v.pre = Extrapolation::Loop;
        QCOMPARE(v.valueAt(5), 20.0);
        QCOMPARE(v.valueAt(0), 10.0);
        QCOMPARE(v.valueAt(-5), 20.0);
        QVERIFY(v.isExtrapolated());
    }

    void spec43_3_pingPong()
    {
        AnimatedValue v = ramp();
        v.post = Extrapolation::PingPong;
        v.pre = Extrapolation::PingPong;
        QCOMPARE(v.valueAtTime(22.5), 25.0); // first mirrored repeat runs 30 -> 10
        QCOMPARE(v.valueAt(30), 10.0);
        QCOMPARE(v.valueAtTime(32.5), 15.0); // second repeat is forward again
        QCOMPARE(v.valueAtTime(7.5), 15.0); // before: the range mirrored at its start
        QCOMPARE(v.valueAt(0), 30.0);
    }

    void spec43_4_relativeAccumulates()
    {
        AnimatedValue v = ramp();
        v.post = Extrapolation::Relative;
        v.pre = Extrapolation::Relative;
        QCOMPARE(v.valueAt(20), 30.0);
        QCOMPARE(v.valueAt(25), 40.0); // repeat 1 starts at 30
        QCOMPARE(v.valueAt(30), 50.0);
        QCOMPARE(v.valueAt(40), 70.0); // repeat 2 ends at 70
        QCOMPARE(v.valueAt(5), 0.0); // one repeat back: 10 - 10 ... starts at -10, ends at 10
        QCOMPARE(v.valueAt(0), -10.0);
    }

    void spec43_5_linearExtrapolation()
    {
        AnimatedValue v = ramp();
        v.post = Extrapolation::Linear;
        v.pre = Extrapolation::Linear;
        QCOMPARE(v.valueAt(30), 50.0); // slope 2 per frame
        QCOMPARE(v.valueAt(0), -10.0);
        // An eased end key has a flat tangent: linear extrapolation then holds the value.
        applyInterp(v.keys.back(), Interp::EaseIn);
        QCOMPARE(v.valueAt(40), 30.0);
    }

    void spec43_6_singleKeyAndNoKeysAreConstant()
    {
        AnimatedValue one(5.0);
        one.setKey(3, 7.0);
        one.post = Extrapolation::Loop;
        QCOMPARE(one.valueAt(50), 7.0);
        AnimatedValue none(5.0);
        none.post = Extrapolation::Relative;
        QCOMPARE(none.valueAt(50), 5.0);
    }

    void spec43_7_loopFollowsEasedKeys()
    {
        AnimatedValue v(0.0);
        v.setKey(0, 0.0, Interp::EaseInOut);
        v.setKey(10, 10.0, Interp::EaseInOut);
        v.post = Extrapolation::Loop;
        QCOMPARE(v.valueAt(13), v.valueAt(3));
        QCOMPARE(v.valueAt(47), v.valueAt(7));
    }

    void spec43_8_stepInAndHold()
    {
        AnimatedValue v = ramp();
        v.keys[1].stepIn = true; // the value jumps right after the first key
        QCOMPARE(v.valueAt(11), 30.0);
        v.keys[1].stepIn = false;
        v.keys[0].hold = true; // stays until the second key
        QCOMPARE(v.valueAt(19), 10.0);
        QCOMPARE(v.valueAt(20), 30.0);
    }

    void spec43_9_reverseAndStretchOnValues()
    {
        AnimatedValue v(0.0);
        v.setKey(0, 0.0, Interp::Linear);
        v.setKey(10, 100.0, Interp::Linear);
        v.setKey(20, 40.0, Interp::Linear);
        const QVector<int64_t> frames {0, 10, 20};
        QCOMPARE(v.reverseKeys(frames), (QVector<int64_t> {0, 10, 20}));
        QCOMPARE(v.valueAt(0), 40.0);
        QCOMPARE(v.valueAt(10), 100.0);
        QCOMPARE(v.valueAt(20), 0.0);
        QCOMPARE(v.stretchKeys(frames, 0.0, 2.0), (QVector<int64_t> {0, 20, 40}));
        QCOMPARE(v.valueAt(20), 100.0);
        QCOMPARE(v.valueAt(30), 50.0);
        // around a pivot: frame 20 stays
        QCOMPARE(v.stretchKeys({0, 20, 40}, 20.0, 0.5), (QVector<int64_t> {10, 20, 30}));
        QCOMPARE(v.valueAt(20), 100.0);
        QCOMPARE(v.valueAt(10), 40.0);
    }

    void spec43_10_reverseMirrorsHandles()
    {
        AnimatedValue v(0.0);
        v.setKey(0, 0.0, Interp::EaseOut); // leaves slowly
        v.setKey(10, 10.0, Interp::Linear);
        const double mid = v.valueAt(2);
        v.reverseKeys({0, 10});
        // The reversed curve arrives slowly at frame 10: value(8) of the mirror equals value(2).
        QCOMPARE(v.valueAt(8), mid);
        QCOMPARE(v.keys[1].in.mode, KeyHandle::Mode::Ease);
    }

    void spec43_11_opsOnTimelineAndNodePath()
    {
        TimelineData d = timeline();
        ClipData c = mediaClip(0, 100);
        Composition comp;
        CompNode n;
        n.id = 5;
        n.type = QStringLiteral("transform");
        n.params.animated(QStringLiteral("angle"), 0).setKey(0, 0.0, Interp::Linear);
        n.params.animated(QStringLiteral("angle"), 0).setKey(10, 90.0, Interp::Linear);
        comp.nodes.append(n);
        c.composition = comp;
        const quint64 id = editops::overwriteClip(d, kV1, c);
        const QString path = ko::nodePath(5, QStringLiteral("angle"));
        QCOMPARE(path, QStringLiteral("node/5/angle"));
        QVERIFY(ko::findValue(*editops::clipById(d, id), path));
        QVERIFY(
            !ko::findValue(*editops::clipById(d, id), ko::nodePath(6, QStringLiteral("angle"))));

        const QVector<KeyframeRef> refs {{id, path, 0}, {id, path, 10}};
        QCOMPARE(ko::setExtrapolation(d, refs, false, true, Extrapolation::Relative), 1);
        const AnimatedValue& v = *ko::findValue(*editops::clipById(d, id), path);
        QCOMPARE(v.post, Extrapolation::Relative);
        QCOMPARE(v.valueAt(20), 180.0);

        QCOMPARE(ko::reverseKeys(d, refs).size(), 2);
        QCOMPARE(ko::findValue(*editops::clipById(d, id), path)->valueAt(0), 90.0);
        QCOMPARE(ko::stretchKeys(d, refs, 0.0, 3.0).size(), 2);
        QCOMPARE(ko::findValue(*editops::clipById(d, id), path)->keys.back().frame, 30);
        const QVector<KeyframeRef> moved {{id, path, 0}, {id, path, 30}};
        QCOMPARE(ko::stepKeys(d, moved, true), 2);
    }

    void spec43_12_serializationRoundTrip()
    {
        ParamSet set;
        AnimatedValue& v = set.animated(QStringLiteral("x"), 0.0);
        v.setKey(0, 1.0);
        v.setKey(10, 5.0);
        v.keys[1].stepIn = true;
        v.pre = Extrapolation::PingPong;
        v.post = Extrapolation::Relative;
        const QJsonObject json = serialization::paramSetToJson(set);
        const ParamSet back = serialization::paramSetFromJson(
            QJsonDocument::fromJson(QJsonDocument(json).toJson()).object());
        QVERIFY(back == set);
        QCOMPARE(back.values.value(QStringLiteral("x")).pre, Extrapolation::PingPong);
        QCOMPARE(back.values.value(QStringLiteral("x")).post, Extrapolation::Relative);
        QVERIFY(back.values.value(QStringLiteral("x")).keys[1].stepIn);
    }

    void spec43_13_oldFilesUnchanged()
    {
        // Nothing in a constant parameter's JSON mentions extrapolation.
        ParamSet set;
        set.animated(QStringLiteral("x"), 0.0).setKey(0, 1.0);
        set.animated(QStringLiteral("x"), 0.0).setKey(4, 2.0);
        const QByteArray text = QJsonDocument(serialization::paramSetToJson(set)).toJson();
        QVERIFY(!text.contains("pre"));
        QVERIFY(!text.contains("post"));
        QVERIFY(!text.contains("stepIn"));
        // A version-18 style value (keys only) loads as constant.
        const QJsonObject old = QJsonDocument::fromJson(
            R"({"values":{"x":{"base":0,"keys":[{"frame":0,"value":1},{"frame":4,"value":2}]}}})")
                                    .object();
        const ParamSet loaded = serialization::paramSetFromJson(old);
        QCOMPARE(loaded.values.value(QStringLiteral("x")).post, Extrapolation::Constant);
        QCOMPARE(loaded.values.value(QStringLiteral("x")).valueAt(100), 2.0);
    }
};

QTEST_GUILESS_MAIN(TestExtrapolation)
#include "test_extrapolation.moc"
