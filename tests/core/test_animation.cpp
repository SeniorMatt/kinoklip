// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for keyframe interpolation (AnimatedValue) and ParamSet helpers.
#include "core/Animation.h"
#include "core/ParamKeys.h"

#include <QTest>

#include <cmath>

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

    // Spec 15.6 / 15.11: presets are handle shapes; ease in and out on both ends of a segment
    // is exactly the smoothstep curve.
    void spec15_11_presetCurves_data()
    {
        QTest::addColumn<int>("first");
        QTest::addColumn<int>("second");
        QTest::addColumn<double>("quarter");
        QTest::addColumn<double>("half");
        QTest::addColumn<double>("threeQuarters");
        const int L = int(Interp::Linear);
        QTest::newRow("linear") << L << L << 25.0 << 50.0 << 75.0;
        // Leaves the first key slowly: controls 0, 0, 200/3, 100 -> 100 (2s^2 - s^3).
        QTest::newRow("easeOutFirst") << int(Interp::EaseOut) << L << 10.9375 << 37.5 << 70.3125;
        // Arrives at the second key slowly: the mirror image.
        QTest::newRow("easeInSecond") << L << int(Interp::EaseIn) << 29.6875 << 62.5 << 89.0625;
        QTest::newRow("easeInOut")
            << int(Interp::EaseInOut) << int(Interp::EaseInOut) << 15.625 << 50.0 << 84.375;
        // An ease-in on the *first* key does not shape the following segment.
        QTest::newRow("easeInFirstIsLinear") << int(Interp::EaseIn) << L << 25.0 << 50.0 << 75.0;
        QTest::newRow("hold") << int(Interp::Hold) << L << 0.0 << 0.0 << 0.0;
    }

    void spec15_11_presetCurves()
    {
        QFETCH(int, first);
        QFETCH(int, second);
        QFETCH(double, quarter);
        QFETCH(double, half);
        QFETCH(double, threeQuarters);
        AnimatedValue v;
        v.setKey(0, 0.0, Interp(first));
        v.setKey(100, 100.0, Interp(second));
        QCOMPARE(v.valueAt(25), quarter);
        QCOMPARE(v.valueAt(50), half);
        QCOMPARE(v.valueAt(75), threeQuarters);
        QCOMPARE(v.valueAt(100), 100.0); // every curve reaches the next key exactly
        QCOMPARE(v.valueAt(0), 0.0);
    }

    void spec15_11_holdJumpsExactlyAtNextKey()
    {
        AnimatedValue v;
        v.setKey(0, 0.0, Interp::Linear);
        v.setKey(10, 10.0, Interp::Hold);
        v.setKey(20, 20.0);
        QCOMPARE(v.valueAt(5), 5.0);
        QCOMPARE(v.valueAt(15), 10.0);
        QCOMPARE(v.valueAt(19), 10.0);
        QCOMPARE(v.valueAtTime(19.999), 10.0);
        QCOMPARE(v.valueAt(20), 20.0);
        QCOMPARE(v.valueAtTime(20.0), 20.0);
        QCOMPARE(keyInterp(v.keys[1]), Interp::Hold);
        // Hold keeps the handles: back to linear restores them.
        applyInterp(v.keys[1], Interp::EaseInOut);
        applyInterp(v.keys[1], Interp::Hold);
        QCOMPARE(v.keys[1].out.mode, KeyHandle::Mode::Ease);
    }

    void spec15_11_smoothTangents()
    {
        AnimatedValue v;
        v.setKey(0, 0.0, Interp::Smooth);
        v.setKey(10, 10.0, Interp::Smooth);
        v.setKey(20, 30.0, Interp::Smooth);
        // Middle slope (30 - 0) / 20 = 1.5; ends are flat. Segment 0..10 controls: 0, 0, 5, 10.
        QCOMPARE(v.valueAt(5), 3.125);
        QCOMPARE(v.inHandle(1), std::make_pair(10.0 / 3.0, 5.0));
        QCOMPARE(v.outHandle(1), std::make_pair(10.0 / 3.0, 5.0));
        // A local extremum gets a flat tangent: no overshoot above the key value.
        AnimatedValue peak;
        peak.setKey(0, 0.0, Interp::Smooth);
        peak.setKey(10, 100.0, Interp::Smooth);
        peak.setKey(20, 0.0, Interp::Smooth);
        for (int f = 0; f <= 20; ++f)
            QVERIFY(peak.valueAt(f) <= 100.0);
    }

    void spec15_11_freeHandlesAreBezier()
    {
        // Free handles at exactly one third reproduce the preset curve...
        AnimatedValue v;
        v.setKey(0, 0.0);
        v.setKey(30, 90.0);
        v.keys[0].out = {KeyHandle::Mode::Free, 10.0, 0.0};
        v.keys[1].in = {KeyHandle::Mode::Free, 10.0, 0.0};
        AnimatedValue ease;
        ease.setKey(0, 0.0, Interp::EaseInOut);
        ease.setKey(30, 90.0, Interp::EaseInOut);
        for (int f = 0; f <= 30; ++f)
            QVERIFY(qAbs(v.valueAt(f) - ease.valueAt(f)) < 1e-6);
        QCOMPARE(keyInterp(v.keys[0]), Interp::Custom);

        // ...and asymmetric handles match a brute-force sampled Bezier.
        v.keys[0].out = {KeyHandle::Mode::Free, 25.0, 60.0};
        v.keys[1].in = {KeyHandle::Mode::Free, 2.0, -20.0};
        const double x1 = 25.0, x2 = 28.0, y1 = 60.0, y2 = 110.0;
        for (int i = 1; i < 2000; ++i) {
            const double s = i / 2000.0, r = 1.0 - s;
            const double x = 3 * r * r * s * x1 + 3 * r * s * s * x2 + s * s * s * 30.0;
            const double y = 3 * r * r * s * y1 + 3 * r * s * s * y2 + s * s * s * 90.0;
            QVERIFY2(qAbs(v.valueAtTime(x) - y) < 1e-6, qPrintable(QString::number(x)));
        }
        QCOMPARE(v.valueAt(0), 0.0);
        QCOMPARE(v.valueAt(30), 90.0);
    }

    void spec15_11_freeHandleClampedToSegment()
    {
        AnimatedValue v;
        v.setKey(0, 0.0);
        v.setKey(10, 10.0);
        v.keys[0].out = {KeyHandle::Mode::Free, 40.0, 40.0}; // 4x too long, slope 1
        QCOMPARE(v.outHandle(0), std::make_pair(10.0, 10.0));
        for (int f = 0; f <= 10; ++f) {
            const double value = v.valueAt(f);
            QVERIFY(std::isfinite(value));
            QVERIFY(value >= 0.0 && value <= 10.0 + 1e-9);
        }
    }

    void spec15_11_boundariesAndFractions()
    {
        AnimatedValue v;
        v.setKey(10, 1.0);
        v.setKey(20, 3.0);
        QCOMPARE(v.valueAtTime(-5.5), 1.0);
        QCOMPARE(v.valueAtTime(10.0), 1.0);
        QCOMPARE(v.valueAtTime(15.5), 2.1);
        QCOMPARE(v.valueAtTime(25.0), 3.0);
        AnimatedValue single;
        single.setKey(4, 9.0);
        QCOMPARE(single.valueAtTime(-1.0), 9.0);
        QCOMPARE(single.valueAtTime(100.0), 9.0);
        QCOMPARE(single.inHandle(0), std::make_pair(0.0, 0.0));
    }

    // Format version 1 meant "interp of the segment that starts here".
    void spec15_11_legacyInterpMigration()
    {
        AnimatedValue v;
        v.setKey(0, 0.0);
        v.setKey(100, 100.0);
        v.setKey(200, 0.0);
        v.setKey(300, 50.0);
        migrateLegacyInterp(v, {Interp::EaseInOut, Interp::EaseOut, Interp::Hold, Interp::Linear});
        QCOMPARE(v.valueAt(25), 15.625); // v1 easeInOut: smoothstep, reproduced exactly
        QCOMPARE(v.valueAt(50), 50.0);
        QCOMPARE(v.keys[0].out.mode, KeyHandle::Mode::Ease);
        QCOMPARE(v.keys[1].in.mode, KeyHandle::Mode::Ease);
        QCOMPARE(v.keys[2].in.mode, KeyHandle::Mode::Ease); // v1 easeOut on key 1
        QVERIFY(v.keys[2].hold);
        QCOMPARE(v.valueAt(299), 0.0);
    }

    void applyAndReadPresets()
    {
        for (Interp i : {Interp::Linear, Interp::EaseIn, Interp::EaseOut, Interp::EaseInOut,
                 Interp::Smooth, Interp::Hold}) {
            Keyframe k;
            applyInterp(k, i);
            QCOMPARE(keyInterp(k), i);
        }
        Keyframe k;
        applyInterp(k, Interp::Custom); // never applied
        QCOMPARE(keyInterp(k), Interp::Linear);
    }

    void setKeyKeepsSortedAndReplaces()
    {
        AnimatedValue v;
        v.setKey(20, 2.0);
        v.setKey(0, 0.0);
        v.setKey(10, 1.0);
        v.setKey(10, 5.0, Interp::EaseIn);
        v.setKey(10, 6.0); // no preset given: the handles stay
        QCOMPARE(v.keys.size(), 3);
        QCOMPARE(v.keys[0].frame, 0);
        QCOMPARE(v.keys[1].frame, 10);
        QCOMPARE(v.keys[1].value, 6.0);
        QCOMPARE(keyInterp(v.keys[1]), Interp::EaseIn);
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
        for (Interp i : {Interp::Linear, Interp::EaseIn, Interp::EaseOut, Interp::EaseInOut,
                 Interp::Hold, Interp::Smooth, Interp::Custom})
            QCOMPARE(interpFromString(interpToString(i)), i);
        for (KeyHandle::Mode m : {KeyHandle::Mode::Linear, KeyHandle::Mode::Ease,
                 KeyHandle::Mode::Smooth, KeyHandle::Mode::Free})
            QCOMPARE(handleModeFromString(handleModeToString(m)), m);
        QCOMPARE(interpFromString(QStringLiteral("bogus")), Interp::Linear);
    }
};

QTEST_GUILESS_MAIN(TestAnimation)
#include "test_animation.moc"
