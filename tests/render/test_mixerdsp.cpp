// SPDX-License-Identifier: GPL-3.0-or-later
// Mixer DSP tests (Audio page, spec 37): EQ frequency response measured with sines,
// compressor gain reduction and attack / release timing, limiter ceiling, EBU R128 loudness
// with synthesized EBU-style test signals, true peak, parameter smoothing.
#include "render/Biquad.h"
#include "render/ChannelProcessor.h"
#include "render/Dynamics.h"
#include "render/Equalizer.h"
#include "render/LoudnessMeter.h"

#include <QTest>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kk;
using namespace kk::dsp;

namespace {

constexpr double kRate = 48000.0;

/// Interleaved stereo sine of @p seconds at @p hz with peak amplitude @p amplitude.
std::vector<float> sine(double hz, double amplitude, double seconds, double phase = 0.0)
{
    const int n = int(seconds * kRate);
    std::vector<float> s(size_t(n) * 2);
    for (int i = 0; i < n; ++i) {
        const float v
            = float(amplitude * std::sin(2.0 * std::numbers::pi * hz * i / kRate + phase));
        s[size_t(i) * 2] = v;
        s[size_t(i) * 2 + 1] = v;
    }
    return s;
}

/// RMS level (dB) of the left channel over frames [from, end).
double rmsDb(const std::vector<float>& s, int from)
{
    double sum = 0.0;
    const int n = int(s.size() / 2);
    for (int i = from; i < n; ++i)
        sum += double(s[size_t(i) * 2]) * s[size_t(i) * 2];
    return 10.0 * std::log10(sum / (n - from));
}

double peakDb(const std::vector<float>& s, int from = 0)
{
    float p = 0.0f;
    for (size_t i = size_t(from) * 2; i < s.size(); ++i)
        p = std::max(p, std::abs(s[i]));
    return 20.0 * std::log10(double(p));
}

/// Gain (dB) the EQ applies to a sine at @p hz, measured after the filters settled.
double measuredGain(const EqSettings& eq, double hz)
{
    Equalizer e(kRate);
    e.setTarget(eq);
    std::vector<float> s = sine(hz, 0.25, 1.0);
    const double in = rmsDb(s, 24000);
    e.process(s.data(), int(s.size() / 2));
    return rmsDb(s, 24000) - in;
}

EqSettings oneBand(EqBandType type, double hz, double gainDb, double q)
{
    EqSettings eq;
    eq.enabled = true;
    for (EqBand& b : eq.bands)
        b.enabled = false;
    eq.bands[2] = EqBand {type, true, hz, gainDb, q};
    return eq;
}

} // namespace

class TestMixerDsp : public QObject
{
    Q_OBJECT

private slots:
    void bellBoostsItsCentreFrequency()
    {
        const EqSettings eq = oneBand(EqBandType::Bell, 1000.0, 6.0, 1.0);
        QVERIFY(std::abs(measuredGain(eq, 1000.0) - 6.0) < 0.1);
        // An octave and more away the bell is nearly flat.
        QVERIFY(std::abs(measuredGain(eq, 100.0)) < 0.3);
        QVERIFY(std::abs(measuredGain(eq, 10000.0)) < 0.3);
        // Half-way: between.
        const double side = measuredGain(eq, 2000.0);
        QVERIFY(side > 1.0 && side < 5.0);
        // The analytic response agrees with the sine measurement.
        const BiquadCoeffs c = designBand(EqBandType::Bell, 1000.0, 6.0, 1.0, kRate);
        QVERIFY(std::abs(magnitudeDb(c, 1000.0, kRate) - 6.0) < 1e-6);
        QVERIFY(std::abs(magnitudeDb(c, 2000.0, kRate) - side) < 0.1);
    }

    void cutAndShelves()
    {
        QVERIFY(std::abs(measuredGain(oneBand(EqBandType::Bell, 3000.0, -12.0, 2.0), 3000.0) + 12.0)
            < 0.1);
        // Shelves: the full gain far into the shelf, half the gain (in dB) at the corner.
        const EqSettings low = oneBand(EqBandType::LowShelf, 200.0, 9.0, 0.7071);
        QVERIFY(std::abs(measuredGain(low, 40.0) - 9.0) < 0.3);
        QVERIFY(std::abs(measuredGain(low, 200.0) - 4.5) < 0.2);
        QVERIFY(std::abs(measuredGain(low, 5000.0)) < 0.1);
        const EqSettings high = oneBand(EqBandType::HighShelf, 4000.0, -6.0, 0.7071);
        QVERIFY(std::abs(measuredGain(high, 16000.0) + 6.0) < 0.3);
        QVERIFY(std::abs(measuredGain(high, 4000.0) + 3.0) < 0.2);
        QVERIFY(std::abs(measuredGain(high, 200.0)) < 0.1);
    }

    void filtersAndNotch()
    {
        // 12 dB/octave Butterworth: -3 dB at the corner, about -12 dB an octave beyond.
        const EqSettings hp = oneBand(EqBandType::HighPass, 400.0, 0.0, 0.7071);
        QVERIFY(std::abs(measuredGain(hp, 400.0) + 3.01) < 0.15);
        QVERIFY(std::abs(measuredGain(hp, 200.0) + 12.3) < 0.5);
        QVERIFY(std::abs(measuredGain(hp, 4000.0)) < 0.1);
        const EqSettings lp = oneBand(EqBandType::LowPass, 2000.0, 0.0, 0.7071);
        QVERIFY(std::abs(measuredGain(lp, 2000.0) + 3.01) < 0.15);
        QVERIFY(std::abs(measuredGain(lp, 100.0)) < 0.1);
        QVERIFY(measuredGain(lp, 8000.0) < -20.0);
        QVERIFY(measuredGain(oneBand(EqBandType::Notch, 1000.0, 0.0, 4.0), 1000.0) < -30.0);
    }

    void neutralAndDisabledEqChangeNothing()
    {
        EqSettings eq; // defaults: every band 0 dB, filters off
        eq.enabled = true;
        QVERIFY(std::abs(measuredGain(eq, 1000.0)) < 1e-4);
        EqSettings off = oneBand(EqBandType::Bell, 1000.0, 12.0, 1.0);
        off.enabled = false;
        Equalizer e(kRate);
        e.setTarget(off);
        std::vector<float> s = sine(1000.0, 0.5, 0.1);
        const std::vector<float> orig = s;
        e.process(s.data(), int(s.size() / 2));
        QCOMPARE(s, orig); // bypassed bit-exactly
    }

    void eqChangesGlideWithoutJumps()
    {
        // A 12 dB step of a bell while a sine plays: the per-sample change of the envelope
        // stays small (the coefficients glide over ~10 ms instead of jumping).
        Equalizer e(kRate);
        EqSettings eq = oneBand(EqBandType::Bell, 1000.0, 0.0, 1.0);
        e.setTarget(eq);
        std::vector<float> s = sine(1000.0, 0.25, 0.2);
        const int half = int(s.size() / 4);
        e.process(s.data(), half);
        eq.bands[2].gainDb = 12.0;
        e.setTarget(eq);
        e.process(s.data() + size_t(half) * 2, half);
        // Right after the change the gain is still on its way (a jump would give +12 dB
        // within one cycle) ...
        auto windowPeakDb = [&s](int from, int to) {
            float p = 0.0f;
            for (int i = from; i < to; ++i)
                p = std::max(p, std::abs(s[size_t(i) * 2]));
            return 20.0 * std::log10(double(p)) - 20.0 * std::log10(0.25);
        };
        const double early = windowPeakDb(half, half + 96); // first 2 ms
        QVERIFY2(early < 6.0, qPrintable(QString::number(early)));
        // ... rises monotonically ...
        QVERIFY(windowPeakDb(half + 240, half + 336) > early);
        // ... and arrives at the full gain.
        QVERIFY(std::abs(windowPeakDb(half + 4000, 2 * half) - 12.0) < 0.2);
    }

    void compressorStaticCurve()
    {
        QCOMPARE(compressorGainReductionDb(-30.0, -20.0, 4.0, 0.0), 0.0);
        QVERIFY(std::abs(compressorGainReductionDb(-10.0, -20.0, 4.0, 0.0) - 7.5) < 1e-9);
        QVERIFY(std::abs(compressorGainReductionDb(0.0, -20.0, 2.0, 0.0) - 10.0) < 1e-9);
        // Soft knee: continuous, smaller than the hard knee inside the knee, equal outside.
        QVERIFY(compressorGainReductionDb(-20.0, -20.0, 4.0, 6.0) > 0.0);
        QVERIFY(compressorGainReductionDb(-24.0, -20.0, 4.0, 6.0) == 0.0);
        QVERIFY(std::abs(compressorGainReductionDb(-10.0, -20.0, 4.0, 6.0) - 7.5) < 1e-9);
    }

    void compressorReducesAndTimesItsEnvelope()
    {
        DynamicsSettings d;
        d.compressorEnabled = true;
        d.thresholdDb = -20.0;
        d.ratio = 4.0;
        d.attackMs = 10.0;
        d.releaseMs = 100.0;
        Dynamics dyn(kRate);
        dyn.setTarget(d);
        // Quiet (-40 dBFS) for 0.5 s: no reduction. Then -10 dBFS: 7.5 dB reduction.
        std::vector<float> quiet = sine(1000.0, std::pow(10.0, -40.0 / 20.0), 0.5);
        dyn.process(quiet.data(), int(quiet.size() / 2));
        QVERIFY(dyn.gainReductionDb() < 0.01f);
        // Square-ish loud signal (DC) so the peak detector sees a constant level.
        const float loud = float(std::pow(10.0, -10.0 / 20.0));
        std::vector<float> step(size_t(kRate) * 2, loud);
        const int attackSamples = int(0.010 * kRate);
        dyn.process(step.data(), attackSamples);
        // After one attack time constant: 63 % of the final 7.5 dB.
        const double afterAttack = dyn.gainReductionDb();
        QVERIFY2(std::abs(afterAttack - 7.5 * (1.0 - std::exp(-1.0))) < 0.15,
            qPrintable(QString::number(afterAttack)));
        dyn.process(step.data() + size_t(attackSamples) * 2, int(kRate) - attackSamples);
        QVERIFY(std::abs(dyn.gainReductionDb() - 7.5) < 0.01);
        // Output level: -10 dB in, 7.5 dB reduction -> -17.5 dBFS.
        QVERIFY(std::abs(20.0 * std::log10(step.back()) + 17.5) < 0.02);
        // Release: back to quiet, after one release time constant 37 % of the reduction left.
        std::vector<float> silent(size_t(0.1 * kRate) * 2, float(std::pow(10.0, -40.0 / 20.0)));
        dyn.process(silent.data(), int(silent.size() / 2));
        QVERIFY2(std::abs(dyn.gainReductionDb() - 7.5 * std::exp(-1.0)) < 0.15,
            qPrintable(QString::number(dyn.gainReductionDb())));
    }

    void makeupGainAndLimiterCeiling()
    {
        DynamicsSettings d;
        d.limiterEnabled = true;
        d.limiterCeilingDb = -6.0;
        Dynamics dyn(kRate);
        dyn.setTarget(d);
        std::vector<float> s = sine(440.0, 1.0, 0.5); // 0 dBFS peaks
        dyn.process(s.data(), int(s.size() / 2));
        QVERIFY2(peakDb(s) <= -6.0 + 0.01, qPrintable(QString::number(peakDb(s))));
        QVERIFY(peakDb(s, 12000) > -6.2); // and not much below it

        DynamicsSettings m;
        m.compressorEnabled = true;
        m.thresholdDb = 0.0; // never reached
        m.makeupDb = 6.0;
        Dynamics up(kRate);
        up.setTarget(m);
        std::vector<float> t = sine(440.0, 0.25, 0.5);
        up.process(t.data(), int(t.size() / 2));
        QVERIFY(std::abs(peakDb(t, 12000) - (20.0 * std::log10(0.25) + 6.0)) < 0.05);
    }

    void loudnessOfEbuSine()
    {
        // EBU Tech 3341 style: a 1 kHz stereo sine at -23 dBFS reads -23 LUFS (momentary,
        // short-term and integrated), and a -33 dBFS one -33 LUFS.
        for (const double level : {-23.0, -33.0}) {
            LoudnessMeter meter;
            const std::vector<float> s = sine(1000.0, std::pow(10.0, level / 20.0), 20.0);
            meter.process(s.data(), int(s.size() / 2));
            QVERIFY2(std::abs(meter.integrated() - level) < 0.1,
                qPrintable(QString::number(meter.integrated())));
            QVERIFY(std::abs(meter.momentary() - level) < 0.1);
            QVERIFY(std::abs(meter.shortTerm() - level) < 0.1);
        }
    }

    void loudnessGating()
    {
        // 10 s at -20 LUFS then 10 s of silence: the absolute gate drops the silence, so the
        // integrated loudness stays -20. A -36 dBFS part (16 LU below) is dropped by the
        // relative gate as well.
        LoudnessMeter meter;
        std::vector<float> loud = sine(1000.0, std::pow(10.0, -20.0 / 20.0), 10.0);
        meter.process(loud.data(), int(loud.size() / 2));
        std::vector<float> silence(size_t(10 * kRate) * 2, 0.0f);
        meter.process(silence.data(), int(silence.size() / 2));
        QVERIFY(std::abs(meter.integrated() + 20.0) < 0.1);
        std::vector<float> soft = sine(1000.0, std::pow(10.0, -36.0 / 20.0), 10.0);
        meter.process(soft.data(), int(soft.size() / 2));
        QVERIFY2(std::abs(meter.integrated() + 20.0) < 0.15,
            qPrintable(QString::number(meter.integrated())));
        QVERIFY(meter.momentary() < -35.0);
        meter.reset();
        QCOMPARE(meter.integrated(), LoudnessMeter::kSilence);
    }

    void truePeakFindsInterSamplePeaks()
    {
        // A sine at fs/4 sampled 45 degrees off its peaks: every sample is at 0.707 (-3 dB),
        // the waveform between them reaches 1.0 (0 dBTP).
        LoudnessMeter meter;
        const std::vector<float> s = sine(12000.0, 1.0, 1.0, std::numbers::pi / 4.0);
        meter.process(s.data(), int(s.size() / 2));
        QVERIFY(std::abs(peakDb(s) + 3.01) < 0.05);
        QVERIFY2(
            std::abs(meter.truePeakMax()) < 0.5, qPrintable(QString::number(meter.truePeakMax())));
        // A low-frequency sine: true peak = sample peak.
        LoudnessMeter low;
        const std::vector<float> t = sine(100.0, 0.5, 1.0);
        low.process(t.data(), int(t.size() / 2));
        QVERIFY(std::abs(low.truePeakMax() - 20.0 * std::log10(0.5)) < 0.05);
    }

    void channelProcessorGainsGlide()
    {
        ChannelProcessor p;
        std::vector<float> s(4800 * 2, 1.0f);
        // First block after reset: the gain is taken as is (no fade-in after a seek).
        p.applyGains(s.data(), 4800, [](int) { return ChannelGains {0.5f, 1.0f, 1.0f}; });
        QCOMPARE(s[0], 0.5f);
        QCOMPARE(s.back(), 0.5f);
        // A jump to 0 glides (no click), reaching silence within ~30 ms.
        std::fill(s.begin(), s.end(), 1.0f);
        p.applyGains(s.data(), 4800, [](int) { return ChannelGains {0.0f, 1.0f, 1.0f}; });
        QVERIFY(s[2] > 0.45f); // 1 sample in: still near 0.5
        float maxStep = 0.0f;
        for (int i = 1; i < 4800; ++i)
            maxStep = std::max(maxStep, std::abs(s[size_t(i) * 2] - s[size_t(i - 1) * 2]));
        QVERIFY(maxStep < 0.01f);
        QVERIFY(s[size_t(1600) * 2] < 1e-3f);
        QCOMPARE(s.back(), 0.0f);
    }
};

QTEST_GUILESS_MAIN(TestMixerDsp)
#include "test_mixerdsp.moc"
