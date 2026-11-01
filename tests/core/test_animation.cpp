// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for keyframe interpolation (AnimatedValue) and ParamSet helpers.
#include "core/Animation.h"
#include "core/ParamKeys.h"

#include <QTest>

using namespace kk;

class TestAnimation : public QObject
{
    Q_OBJECT
private slots:
    void baseWithoutKeys()
    {
        AnimatedValue v(42.0);
        QVERIFY(!v.isAnimated());
        QCOMPARE(v.valueAt(-100), 42.0);
        QCOMPARE(v.valueAt(1000), 42.0);
    }

    void holdsOutsideKeyRange()
    {
        AnimatedValue v(5.0);
        v.setKey(10, 1.0);
        v.setKey(20, 3.0);
        QCOMPARE(v.valueAt(0), 1.0);
        QCOMPARE(v.valueAt(10), 1.0);
        QCOMPARE(v.valueAt(20), 3.0);
        QCOMPARE(v.valueAt(99), 3.0);
    }

    void linear()
    {
        AnimatedValue v;
        v.setKey(0, 0.0);
        v.setKey(10, 100.0);
        QCOMPARE(v.valueAt(5), 50.0);
        QCOMPARE(v.valueAt(1), 10.0);
    }

    void easingCurves_data()
    {
        QTest::addColumn<int>("interp");
        QTest::addColumn<double>("quarter");
        QTest::addColumn<double>("half");
        QTest::newRow("linear") << int(Interp::Linear) << 25.0 << 50.0;
        QTest::newRow("easeIn") << int(Interp::EaseIn) << 6.25 << 25.0;
        QTest::newRow("easeOut") << int(Interp::EaseOut) << 43.75 << 75.0;
        QTest::newRow("easeInOut") << int(Interp::EaseInOut) << 15.625 << 50.0;
        QTest::newRow("hold") << int(Interp::Hold) << 0.0 << 0.0;
    }

    void easingCurves()
    {
        QFETCH(int, interp);
        QFETCH(double, quarter);
        QFETCH(double, half);
        AnimatedValue v;
        v.setKey(0, 0.0, Interp(interp));
        v.setKey(100, 100.0);
        QCOMPARE(v.valueAt(25), quarter);
        QCOMPARE(v.valueAt(50), half);
        QCOMPARE(v.valueAt(100), 100.0); // every curve reaches the next key exactly
        QCOMPARE(v.valueAt(0), 0.0);
    }

    void segmentUsesInterpOfLeftKey()
    {
        AnimatedValue v;
        v.setKey(0, 0.0, Interp::Linear);
        v.setKey(10, 10.0, Interp::Hold);
        v.setKey(20, 20.0);
        QCOMPARE(v.valueAt(5), 5.0);
        QCOMPARE(v.valueAt(15), 10.0);
        QCOMPARE(v.valueAt(19), 10.0);
        QCOMPARE(v.valueAt(20), 20.0);
    }

    void setKeyKeepsSortedAndReplaces()
    {
        AnimatedValue v;
        v.setKey(20, 2.0);
        v.setKey(0, 0.0);
        v.setKey(10, 1.0);
        v.setKey(10, 5.0, Interp::EaseIn);
        QCOMPARE(v.keys.size(), 3);
        QCOMPARE(v.keys[0].frame, 0);
        QCOMPARE(v.keys[1].frame, 10);
        QCOMPARE(v.keys[1].value, 5.0);
        QCOMPARE(v.keys[1].interp, Interp::EaseIn);
        QCOMPARE(v.keys[2].frame, 20);
        QVERIFY(v.hasKeyAt(10));
        QVERIFY(!v.hasKeyAt(11));
    }

    void removeKeyKeepsLastValue()
    {
        AnimatedValue v(1.0);
        v.setKey(5, 7.0);
        v.setKey(9, 8.0);
        v.removeKey(9);
        QCOMPARE(v.keys.size(), 1);
        v.removeKey(3); // no key there: no-op
        QCOMPARE(v.keys.size(), 1);
        v.removeKey(5);
        QVERIFY(!v.isAnimated());
        QCOMPARE(v.base, 7.0);
        QCOMPARE(v.valueAt(0), 7.0);
    }

    void setValueEditsBaseOrKey()
    {
        AnimatedValue v(1.0);
        v.setValue(10, 2.0);
        QCOMPARE(v.base, 2.0);
        QVERIFY(!v.isAnimated());
        v.setKey(0, 0.0);
        v.setValue(10, 3.0);
        QVERIFY(v.hasKeyAt(10));
        QCOMPARE(v.valueAt(10), 3.0);
    }

    void keyNavigation()
    {
        AnimatedValue v;
        v.setKey(10, 0.0);
        v.setKey(20, 0.0);
        QCOMPARE(v.previousKeyFrame(10), std::nullopt);
        QCOMPARE(v.previousKeyFrame(15), std::optional<int64_t>(10));
        QCOMPARE(v.previousKeyFrame(25), std::optional<int64_t>(20));
        QCOMPARE(v.nextKeyFrame(10), std::optional<int64_t>(20));
        QCOMPARE(v.nextKeyFrame(0), std::optional<int64_t>(10));
        QCOMPARE(v.nextKeyFrame(20), std::nullopt);
    }

    void shiftKeys()
    {
        AnimatedValue v;
        v.setKey(10, 1.0);
        v.setKey(20, 2.0);
        v.shiftKeys(-10);
        QCOMPARE(v.keys[0].frame, 0);
        QCOMPARE(v.valueAt(5), 1.5);
    }

    void paramSetDefaults()
    {
        ParamSet p;
        QCOMPARE(p.value(keys::Opacity, 0), 100.0);
        QCOMPARE(p.value(keys::ZoomX, 0), 1.0);
        QCOMPARE(p.value(keys::PosX, 0), 0.0);
        QCOMPARE(p.value(QStringLiteral("unknown"), 0, 3.5), 3.5);
        p.setBase(keys::Opacity, 50.0);
        QCOMPARE(p.value(keys::Opacity, 0), 50.0);
        QVERIFY(!p.isAnimated());
        p.animated(keys::PosX, 0).setKey(0, 10.0);
        QVERIFY(p.isAnimated());
        QCOMPARE(p.staticValue(QStringLiteral("x"), true).toBool(), true);
    }

    void interpNamesRoundTrip()
    {
        for (Interp i :
            {Interp::Linear, Interp::EaseIn, Interp::EaseOut, Interp::EaseInOut, Interp::Hold})
            QCOMPARE(interpFromString(interpToString(i)), i);
        QCOMPARE(interpFromString(QStringLiteral("bogus")), Interp::Linear);
    }
};

QTEST_GUILESS_MAIN(TestAnimation)
#include "test_animation.moc"
