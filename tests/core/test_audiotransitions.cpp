// SPDX-License-Identifier: GPL-3.0-or-later
// Audio transitions: the six gain curves, the registry entries and project save / load.
#include "core/AudioTransitions.h"
#include "core/EffectRegistry.h"
#include "core/Serialization.h"

#include <QJsonDocument>
#include <QTest>

#include <cmath>

using namespace kk;
namespace at = kk::audiotransitions;

class TestAudioTransitions : public QObject
{
    Q_OBJECT
private slots:
    void spec13_12_gainCurves_data()
    {
        QTest::addColumn<int>("curve");
        QTest::addColumn<double>("mid"); // gainIn(0.5) = gainOut(0.5)
        QTest::newRow("0 dB linear") << int(at::Curve::Linear) << 0.5;
        QTest::newRow("-3 dB") << int(at::Curve::Minus3dB) << 0.5 * (0.5 + std::sqrt(0.5));
        QTest::newRow("+3 dB equal power") << int(at::Curve::EqualPower) << std::sqrt(0.5);
        QTest::newRow("exponential")
            << int(at::Curve::Exponential) << std::expm1(2.0) / std::expm1(4.0);
        QTest::newRow("logarithmic") << int(at::Curve::Logarithmic) << std::log10(5.5);
        QTest::newRow("s-curve") << int(at::Curve::SCurve) << 0.5;
    }

    void spec13_12_gainCurves()
    {
        QFETCH(int, curve);
        QFETCH(double, mid);
        const auto c = at::Curve(curve);
        QCOMPARE(at::gainIn(c, 0.0), 0.0);
        QCOMPARE(at::gainIn(c, 1.0), 1.0);
        QCOMPARE(at::gainOut(c, 0.0), 1.0);
        QCOMPARE(at::gainOut(c, 1.0), 0.0);
        QVERIFY(std::abs(at::gainIn(c, 0.5) - mid) < 1e-9);
        QVERIFY(std::abs(at::gainOut(c, 0.5) - mid) < 1e-9);
        // Monotonic, inside [0, 1], mirrored, clamped outside [0, 1].
        double last = -1.0;
        for (int i = 0; i <= 100; ++i) {
            const double t = i / 100.0;
            const double g = at::gainIn(c, t);
            QVERIFY(g >= last && g >= 0.0 && g <= 1.0);
            QVERIFY(std::abs(at::gainOut(c, t) - at::gainIn(c, 1.0 - t)) < 1e-12);
            last = g;
        }
        QCOMPARE(at::gainIn(c, -0.5), 0.0);
        QCOMPARE(at::gainIn(c, 1.5), 1.0);
    }

    void spec13_12_idsAndRegistry()
    {
        const auto list = EffectRegistry::instance().byCategory(EffectCategory::AudioTransition);
        QCOMPARE(list.size(), 6);
        for (const EffectDescriptor* d : list)
            QVERIFY(at::isAudioTransition(d->id));
        QCOMPARE(at::defaultId(), QStringLiteral("audio_crossfade_3db"));
        QCOMPARE(at::curveFor(QStringLiteral("audio_crossfade_0db")), at::Curve::Linear);
        QCOMPARE(at::curveFor(QStringLiteral("audio_crossfade_3db")), at::Curve::EqualPower);
        // Video ids (older projects store "cross_dissolve" on audio clips) and unknown ids:
        // the +3 dB equal-power crossfade.
        QCOMPARE(at::curveFor(QStringLiteral("cross_dissolve")), at::Curve::EqualPower);
        QCOMPARE(at::curveFor(QStringLiteral("wipe")), at::Curve::EqualPower);
        QCOMPARE(at::curveFor(QString()), at::Curve::EqualPower);
        QVERIFY(!at::isAudioTransition(QStringLiteral("cross_dissolve")));
        // No overlap with the video transitions.
        for (const EffectDescriptor* d :
            EffectRegistry::instance().byCategory(EffectCategory::Transition))
            QVERIFY(!at::isAudioTransition(d->id));
    }

    void spec13_16_saveLoadRoundTrip()
    {
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = QStringLiteral("m1");
        clip.duration = 100;
        clip.transitionIn = EffectRegistry::instance().createTransition(
            QStringLiteral("audio_crossfade_exponential"), 12);
        clip.transitionOut = EffectRegistry::instance().createTransition(
            QStringLiteral("audio_crossfade_0db"), 30);
        clip.transitionOut->alignment = TransitionAlignment::EndOnCut;
        const QJsonDocument doc
            = QJsonDocument::fromJson(QJsonDocument(serialization::clipToJson(clip)).toJson());
        const ClipData restored = serialization::clipFromJson(doc.object());
        QCOMPARE(restored, clip);
        QCOMPARE(restored.transitionIn->type, QStringLiteral("audio_crossfade_exponential"));
        QCOMPARE(restored.transitionOut->alignment, TransitionAlignment::EndOnCut);

        // An older project: a video id on an audio clip loads unchanged and plays +3 dB.
        ClipData old = clip;
        old.transitionIn = TransitionData {QStringLiteral("cross_dissolve"), 10, {}};
        old.transitionOut.reset();
        const ClipData loaded = serialization::clipFromJson(serialization::clipToJson(old));
        QCOMPARE(loaded, old);
        QCOMPARE(at::curveFor(loaded.transitionIn->type), at::Curve::EqualPower);
    }
};

QTEST_GUILESS_MAIN(TestAudioTransitions)
#include "test_audiotransitions.moc"
