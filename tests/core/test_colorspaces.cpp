// SPDX-License-Identifier: GPL-3.0-or-later
// The colour science of the colour management effects (docs/behavior-spec.md section 205):
// matrices built from primaries against published values, chromatic adaptation, the transfer
// curves at their well-known points and as round trips, ACES RRT + ODT fit and its inverse,
// tone mapping, gamut compression.
#include "core/ColorSpaces.h"

#include <QTest>

#include <cmath>

using namespace kk::cm;

namespace {

void near(double a, double b, double tol, const char* what)
{
    if (std::abs(a - b) > tol)
        QFAIL(qPrintable(QStringLiteral("%1: %2 vs %3").arg(what).arg(a).arg(b)));
}

void nearMat(const Mat3& m, const Mat3& want, double tol, const char* what)
{
    for (size_t i = 0; i < 9; ++i)
        near(m[i], want[i], tol, what);
}

} // namespace

class TestColorSpaces : public QObject
{
    Q_OBJECT
private slots:
    void rec709ToXyz()
    {
        nearMat(rgbToXyz(Gamut::Rec709),
            {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339,
                0.1191920, 0.9503041},
            1e-3, "sRGB to XYZ");
        nearMat(rgbToXyz(Gamut::Rec2020),
            {0.6369580, 0.1446169, 0.1688810, 0.2627002, 0.6779981, 0.0593017, 0.0, 0.0280727,
                1.0609851},
            1e-3, "Rec.2020 to XYZ");
        // ACES AP0 to XYZ is the one in SMPTE ST 2065-1.
        nearMat(rgbToXyz(Gamut::AcesAp0),
            {0.9525523959, 0.0, 0.0000936786, 0.3439664498, 0.7281660966, -0.0721325464,
                0.0, 0.0, 1.0088251844},
            1e-3, "AP0 to XYZ");
    }

    void srgbToAcesCg()
    {
        // The well-known Rec.709 (D65) to ACEScg matrix, with a Bradford D65 -> D60 adaptation.
        nearMat(gamutMatrix(Gamut::Rec709, Gamut::AcesAp1, Cat::Bradford, true),
            {0.6131, 0.3395, 0.0474, 0.0702, 0.9164, 0.0134, 0.0206, 0.1096, 0.8698}, 6e-4,
            "sRGB to ACEScg");
        // White stays white: the D65 white is the D60 white of ACES.
        const Vec3 w = mulVec(gamutMatrix(Gamut::Rec709, Gamut::AcesAp1, Cat::Bradford, true),
            {1, 1, 1});
        near(w[0], 1.0, 2e-3, "white r");
        near(w[1], 1.0, 2e-3, "white g");
        near(w[2], 1.0, 2e-3, "white b");
        // Without adaptation a D65 white is not white in AP1 (it has a D60 white).
        const Vec3 raw = mulVec(gamutMatrix(Gamut::Rec709, Gamut::AcesAp1, Cat::Bradford, false),
            {1, 1, 1});
        QVERIFY(std::abs(raw[0] - raw[2]) > 0.02);
    }

    void bradfordD65ToD50()
    {
        // Lindbloom's published Bradford matrix.
        nearMat(adaptation(Cat::Bradford, whiteOf(WhitePoint::D65), whiteOf(WhitePoint::D50)),
            {1.0478112, 0.0228866, -0.0501270, 0.0295424, 0.9904844, -0.0170491, -0.0092345,
                0.0150436, 0.7521316},
            2e-3, "Bradford D65 to D50");
        const Mat3 same = adaptation(Cat::Cat02, whiteOf(WhitePoint::D65), whiteOf(WhitePoint::D65));
        nearMat(same, {1, 0, 0, 0, 1, 0, 0, 0, 1}, 1e-9, "same white");
        // Every method moves the source white exactly to the target white.
        for (Cat c : {Cat::Bradford, Cat::Cat02, Cat::VonKries}) {
            const Vec3 w = mulVec(adaptation(c, whiteOf(WhitePoint::D65), whiteOf(WhitePoint::IlluminantA)),
                xyzOf(whiteOf(WhitePoint::D65)));
            const Vec3 t = xyzOf(whiteOf(WhitePoint::IlluminantA));
            near(w[0], t[0], 1e-6, "white x");
            near(w[2], t[2], 1e-6, "white z");
        }
        // The methods differ from each other.
        const Mat3 a = adaptation(Cat::Bradford, whiteOf(WhitePoint::D65), whiteOf(WhitePoint::D50));
        const Mat3 b = adaptation(Cat::VonKries, whiteOf(WhitePoint::D65), whiteOf(WhitePoint::D50));
        QVERIFY(std::abs(a[0] - b[0]) > 1e-3);
    }

    void transferPoints()
    {
        // 18 % grey in every curve (published values).
        near(encode(Transfer::Srgb, 0.18f), 0.4614, 1e-3, "sRGB");
        near(encode(Transfer::Rec709, 0.18f), 0.4090, 1e-3, "Rec.709");
        near(encode(Transfer::AcesCc, 0.18f), 0.4135884, 1e-5, "ACEScc");
        near(encode(Transfer::AcesCct, 0.18f), 0.4135884, 1e-5, "ACEScct");
        near(encode(Transfer::LogC3, 0.18f), 0.391007, 1e-4, "LogC3");
        near(encode(Transfer::SLog3, 0.18f), 420.0 / 1023.0, 1e-5, "S-Log3");
        near(encode(Transfer::VLog, 0.18f), 0.423311, 1e-4, "V-Log");
        near(encode(Transfer::Log3G10, 0.18f), 1.0 / 3.0, 1e-3, "Log3G10");
        near(encode(Transfer::DavinciIntermediate, 0.18f), 0.336, 1e-3, "DaVinci Intermediate");
        // PQ: 100 nits (1.0 here) is code 0.508078; 10000 nits is 1.0; 0 is 0.
        near(encode(Transfer::Pq, 1.f), 0.508078, 1e-5, "PQ 100 nits");
        near(encode(Transfer::Pq, 100.f), 1.0, 1e-6, "PQ 10000 nits");
        near(encode(Transfer::Pq, 0.f), 0.0, 1e-6, "PQ black");
        near(decode(Transfer::Pq, 0.508078f), 1.0, 2e-3, "PQ decode");
        // HLG: scene 1/12 is signal 0.5, full scene is signal 1.
        near(encode(Transfer::Hlg, 1.f / 12.f), 0.5, 1e-6, "HLG half");
        near(encode(Transfer::Hlg, 1.f), 1.0, 1e-3, "HLG one");
        // Gammas.
        near(decode(Transfer::Gamma24, 0.5f), std::pow(0.5, 2.4), 1e-6, "gamma 2.4");
        near(decode(Transfer::Gamma26, 0.5f), std::pow(0.5, 2.6), 1e-6, "gamma 2.6");
    }

    void transferRoundTrips()
    {
        for (int t = 0; t < int(Transfer::Count); ++t) {
            const Transfer tf = Transfer(t);
            for (float lin : {0.0005f, 0.01f, 0.05f, 0.18f, 0.5f, 0.9f}) {
                // PQ and HLG are display curves with a limited range: stay inside it.
                const float back = decode(tf, encode(tf, lin));
                if (std::abs(back - lin) > std::max(2e-4f * lin, 2e-5f))
                    QFAIL(qPrintable(QStringLiteral("%1 at %2 gave %3")
                                         .arg(transferEntry(tf).id)
                                         .arg(double(lin))
                                         .arg(double(back))));
            }
        }
    }

    void idsAreUnique()
    {
        for (int i = 0; i < int(Gamut::Count); ++i)
            QCOMPARE(int(gamutFromId(QLatin1String(gamutEntry(Gamut(i)).id), Gamut::Count)), i);
        for (int i = 0; i < int(Transfer::Count); ++i)
            QCOMPARE(
                int(transferFromId(QLatin1String(transferEntry(Transfer(i)).id), Transfer::Count)),
                i);
        for (int i = 0; i < int(WhitePoint::Count); ++i)
            QCOMPARE(int(whitePointFromId(
                         QLatin1String(whitePointEntry(WhitePoint(i)).id), WhitePoint::Count)),
                i);
        QVERIFY(acesEntryFromId(QStringLiteral("acescg")));
        QVERIFY(!acesEntryFromId(QStringLiteral("nothing")));
    }

    void colorSpaceTransform()
    {
        // Rec.709 / gamma 2.4 into itself changes nothing.
        const CstParams same = makeCst(Gamut::Rec709, Transfer::Gamma24, Gamut::Rec709,
            Transfer::Gamma24, ToneMap::None, GamutMap::None, false, false, true, 1000, 100);
        const float in[3] = {0.2f, 0.5f, 0.8f};
        float out[3];
        cstApply(same, in, out);
        for (int i = 0; i < 3; ++i)
            near(out[i], in[i], 1e-5, "identity");
        // DaVinci Intermediate grey 18 % becomes gamma 2.4 grey 18 %.
        const CstParams di = makeCst(Gamut::DavinciWide, Transfer::DavinciIntermediate,
            Gamut::Rec709, Transfer::Gamma24, ToneMap::None, GamutMap::None, false, false, true,
            1000, 100);
        const float grey[3] = {encode(Transfer::DavinciIntermediate, 0.18f),
            encode(Transfer::DavinciIntermediate, 0.18f),
            encode(Transfer::DavinciIntermediate, 0.18f)};
        cstApply(di, grey, out);
        for (int i = 0; i < 3; ++i)
            near(out[i], std::pow(0.18, 1.0 / 2.4), 2e-3, "DI grey");
        // Linear Rec.709 white into ACEScg stays 1 with white point adaptation.
        const CstParams cg = makeCst(Gamut::Rec709, Transfer::Linear, Gamut::AcesAp1,
            Transfer::Linear, ToneMap::None, GamutMap::None, false, false, true, 1000, 100);
        const float white[3] = {1, 1, 1};
        cstApply(cg, white, out);
        for (int i = 0; i < 3; ++i)
            near(out[i], 1.0, 2e-3, "white");
    }

    void toneMapping()
    {
        // PQ 1000 nits (10 units) into a 100 nit display: bright values fit, shadows stay.
        auto run = [](ToneMap tone, float code) {
            const CstParams p = makeCst(Gamut::Rec2020, Transfer::Pq, Gamut::Rec2020,
                Transfer::Linear, tone, GamutMap::None, false, false, true, 1000, 100);
            const float in[3] = {code, code, code};
            float out[3];
            cstApply(p, in, out);
            return out[0];
        };
        const float peak = encode(Transfer::Pq, 10.f); // 1000 nits
        const float dark = encode(Transfer::Pq, 0.05f);
        for (ToneMap t : {ToneMap::Simple, ToneMap::Luminance, ToneMap::Davinci}) {
            near(run(t, peak), 1.0, 1e-2, "peak maps to the output peak");
            QVERIFY(run(t, dark) <= 0.05f + 1e-4f);
            QVERIFY(run(t, dark) > 0.04f);
        }
        QVERIFY(run(ToneMap::None, peak) > 9.5f); // untouched without tone mapping
    }

    void hlgOotf()
    {
        // 1000 nit HLG: scene white 1.0 becomes display white of 10 units, with and without the
        // OOTF being undone again by the inverse.
        const CstParams fwd = makeCst(Gamut::Rec2020, Transfer::Hlg, Gamut::Rec2020,
            Transfer::Linear, ToneMap::None, GamutMap::None, true, false, true, 1000, 100);
        const float white[3] = {1, 1, 1};
        float out[3];
        cstApply(fwd, white, out);
        near(out[1], 10.0, 0.05, "HLG white at 1000 nits");
        const CstParams back = makeCst(Gamut::Rec2020, Transfer::Linear, Gamut::Rec2020,
            Transfer::Hlg, ToneMap::None, GamutMap::None, false, true, true, 1000, 1000);
        const float disp[3] = {5.f, 5.f, 5.f};
        float scene[3];
        cstApply(back, disp, scene);
        const CstParams fwd2 = makeCst(Gamut::Rec2020, Transfer::Hlg, Gamut::Rec2020,
            Transfer::Linear, ToneMap::None, GamutMap::None, true, false, true, 1000, 1000);
        float again[3];
        cstApply(fwd2, scene, again);
        near(again[1], 5.0, 0.02, "OOTF round trip");
    }

    void acesTransform()
    {
        const AcesEntry& acescg = *acesEntryFromId(QStringLiteral("acescg"));
        const AcesEntry& srgb = *acesEntryFromId(QStringLiteral("srgb"));
        const AcesEntry& acescct = *acesEntryFromId(QStringLiteral("acescct"));
        // sRGB IDT into ACEScg: no tone curve, just linearise and move the primaries.
        const CstParams idt = makeAces(srgb, acescg, false);
        const float in[3] = {0.5f, 0.5f, 0.5f};
        float out[3];
        cstApply(idt, in, out);
        const float lin = decode(Transfer::Srgb, 0.5f);
        for (int i = 0; i < 3; ++i)
            near(out[i], lin, 3e-3, "sRGB IDT grey");
        // ACEScg to ACEScct: the curve only.
        cstApply(makeAces(acescg, acescct, false), in, out);
        near(out[0], encode(Transfer::AcesCct, 0.5f), 1e-5, "ACEScct");
        // ACEScg 18 % grey through the RRT + ODT to sRGB: a mid-grey code value that is neither
        // clipped nor lifted (the real transform gives about 0.46).
        const float grey[3] = {0.18f, 0.18f, 0.18f};
        cstApply(makeAces(acescg, srgb, false), grey, out);
        QVERIFY2(out[1] > 0.35f && out[1] < 0.6f, qPrintable(QString::number(double(out[1]))));
        near(out[0], out[1], 5e-3, "grey stays grey");
        near(out[2], out[1], 5e-3, "grey stays grey (blue)");
        // The output is bounded to the display range and black stays black.
        const float bright[3] = {50.f, 50.f, 50.f};
        cstApply(makeAces(acescg, srgb, false), bright, out);
        QVERIFY(out[1] <= 1.0f);
        const float black[3] = {0.f, 0.f, 0.f};
        cstApply(makeAces(acescg, srgb, false), black, out);
        near(out[1], 0.0, 1e-3, "black");
        // The inverse output transform brings the display picture back.
        const float disp[3] = {0.6f, 0.4f, 0.3f};
        float scene[3];
        cstApply(makeAces(srgb, acescg, true), disp, scene);
        float display[3];
        cstApply(makeAces(acescg, srgb, false), scene, display);
        for (int i = 0; i < 3; ++i)
            near(display[i], disp[i], 5e-3, "ODT round trip");
        // RRT + ODT fit and its inverse.
        const float ac[3] = {0.18f, 0.4f, 0.05f};
        float d[3], ac2[3];
        rrtOdtFit(ac, d);
        rrtOdtFitInverse(d, ac2);
        for (int i = 0; i < 3; ++i)
            near(ac2[i], ac[i], 1e-3, "fit inverse");
    }

    void gamutCompression()
    {
        near(compress(0.5f, 0.8f, 1.2f, 1.2f), 0.5, 1e-9, "below the threshold");
        near(compress(1.2f, 0.8f, 1.2f, 1.2f), 1.0, 1e-4, "limit maps to one");
        QVERIFY(compress(1.0f, 0.8f, 1.2f, 1.2f) < 1.0f);
        QVERIFY(compress(1.0f, 0.8f, 1.2f, 1.2f) > 0.8f);
        // A Rec.2020 green in Rec.709 is negative; compression brings every channel into range.
        const CstParams p = makeGamutMapping(Gamut::Rec2020, Transfer::Linear, Gamut::Rec709,
            GamutMap::Compression, 0.8, 1.2, 1.2, 1.0);
        const float green[3] = {0.f, 1.f, 0.f};
        float out[3];
        // A colour within the compression limit ends inside Rec.709 (seen in Rec.709 again,
        // no channel is negative); it was outside before.
        const Mat3 to709 = gamutMatrix(Gamut::Rec2020, Gamut::Rec709, Cat::Bradford, true);
        const float outside[3] = {0.15f, 0.5f, 0.2f};
        const Vec3 before = mulVec(to709, {outside[0], outside[1], outside[2]});
        QVERIFY(before[0] < -0.03);
        cstApply(p, outside, out);
        const Vec3 v = mulVec(to709, {out[0], out[1], out[2]});
        QVERIFY2(v[0] > -1e-3 && v[1] > -1e-3 && v[2] > -1e-3, qPrintable(QString::number(v[0])));
        // Colors inside the target stay (compression threshold).
        const float inside[3] = {0.3f, 0.4f, 0.3f};
        cstApply(p, inside, out);
        for (int i = 0; i < 3; ++i)
            near(out[i], inside[i], 1e-3, "inside stays");
        // Saturation mapping also removes negatives.
        const CstParams s = makeGamutMapping(Gamut::Rec2020, Transfer::Linear, Gamut::Rec709,
            GamutMap::Saturation, 0.8, 1.2, 1.2, 1.0);
        cstApply(s, green, out);
        const Vec3 v2 = mulVec(to709, {out[0], out[1], out[2]});
        QVERIFY(v2[0] > -1e-4 && v2[1] > -1e-4 && v2[2] > -1e-4);
    }

    void chromaticAdaptationAndCompressor()
    {
        const CstParams ca = makeAdaptation(Gamut::Rec709, Transfer::Linear, Cat::Bradford,
            whiteOf(WhitePoint::D65), whiteOf(WhitePoint::D50), 1.0);
        const float white[3] = {1, 1, 1};
        float out[3];
        cstApply(ca, white, out);
        // D65 white seen under D50: warmer, so red above blue.
        QVERIFY(out[0] > out[2]);
        near(out[1], 1.0, 0.05, "luminance holds");
        const CstParams cc = makeCompressor(Gamut::Rec709, Transfer::Linear, 0.8, 2.0, 0.8, 1.2, 1.0);
        const float hot[3] = {2.0f, 0.5f, 0.5f};
        cstApply(cc, hot, out);
        QVERIFY(out[0] <= 1.0001f);
        const float low[3] = {0.5f, 0.4f, 0.3f};
        cstApply(cc, low, out);
        near(out[0], 0.5, 1e-4, "below the threshold stays");
    }
};

QTEST_GUILESS_MAIN(TestColorSpaces)
#include "test_colorspaces.moc"
