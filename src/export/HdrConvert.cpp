// SPDX-License-Identifier: GPL-3.0-or-later
#include "export/HdrConvert.h"

#include <algorithm>
#include <cmath>

namespace kk::exporter {

namespace {

constexpr int kEncodeSteps = 4096;

// BT.709 to BT.2020 primaries on linear light (ITU-R BT.2087).
constexpr float kM[3][3] = {{0.6274039f, 0.3292830f, 0.0433131f},
    {0.0690973f, 0.9195404f, 0.0113623f}, {0.0163914f, 0.0880133f, 0.8955953f}};

} // namespace

double pqEncode(double nits)
{
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double c3 = 2392.0 / 4096.0 * 32.0;
    const double y = std::pow(std::clamp(nits, 0.0, 10000.0) / 10000.0, m1);
    return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

double hlgEncode(double e)
{
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    e = std::clamp(e, 0.0, 1.0);
    return e <= 1.0 / 12.0 ? std::sqrt(3.0 * e) : a * std::log(12.0 * e - b) + c;
}

double srgbDecode(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double srgbEncode(double l)
{
    return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

SdrToHdr::SdrToHdr(HdrTransfer transfer, bool rec2020)
    : m_rec2020(rec2020 || transfer != HdrTransfer::Sdr)
{
    for (int i = 0; i < 256; ++i)
        m_linear[size_t(i)] = float(srgbDecode(i / 255.0));
    // Output curve over linear light 0..1 (1 = SDR white). Indexed by sqrt(linear) so the
    // steep part near black keeps enough steps; entries hold the 16-bit signal.
    m_encode.resize(kEncodeSteps + 1);
    for (int i = 0; i <= kEncodeSteps; ++i) {
        const double s = double(i) / kEncodeSteps;
        const double lin = s * s;
        double signal = 0.0;
        switch (transfer) {
        case HdrTransfer::Pq:
            signal = pqEncode(lin * kHdrReferenceWhiteNits);
            break;
        case HdrTransfer::Hlg:
            signal = hlgEncode(lin * kHlgReferenceWhiteScene);
            break;
        case HdrTransfer::Sdr:
            signal = srgbEncode(lin);
            break;
        }
        m_encode[size_t(i)] = float(std::clamp(signal, 0.0, 1.0) * 65535.0);
    }
}

void SdrToHdr::convertRow(const QRgb* in, uint16_t* out, int width) const
{
    const float* lin = m_linear.data();
    const float* enc = m_encode.data();
    auto curve = [enc](float v) {
        const float x = std::sqrt(std::clamp(v, 0.0f, 1.0f)) * float(kEncodeSteps);
        const int i = std::min(int(x), kEncodeSteps - 1);
        const float f = x - float(i);
        return uint16_t(enc[i] + (enc[i + 1] - enc[i]) * f + 0.5f);
    };
    for (int x = 0; x < width; ++x) {
        const QRgb px = in[x];
        float r = lin[qRed(px)];
        float g = lin[qGreen(px)];
        float b = lin[qBlue(px)];
        if (m_rec2020) {
            const float r2 = kM[0][0] * r + kM[0][1] * g + kM[0][2] * b;
            const float g2 = kM[1][0] * r + kM[1][1] * g + kM[1][2] * b;
            const float b2 = kM[2][0] * r + kM[2][1] * g + kM[2][2] * b;
            r = r2;
            g = g2;
            b = b2;
        }
        out[3 * x] = curve(r);
        out[3 * x + 1] = curve(g);
        out[3 * x + 2] = curve(b);
    }
}

std::array<uint16_t, 3> SdrToHdr::convertPixel(int r, int g, int b) const
{
    const QRgb px = qRgb(r, g, b);
    std::array<uint16_t, 3> out {};
    convertRow(&px, out.data(), 1);
    return out;
}

QByteArray x265MasterDisplay(const HdrMetadata& hdr)
{
    // P3-D65: G(0.265, 0.690) B(0.150, 0.060) R(0.680, 0.320) WP(0.3127, 0.3290); BT.2020:
    // G(0.170, 0.797) B(0.131, 0.046) R(0.708, 0.292). Units of 0.00002.
    const QByteArray primaries = hdr.primaries == MasterPrimaries::Rec2020
        ? "G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)"
        : "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)";
    return primaries + "L("
        + QByteArray::number(qlonglong(std::llround(hdr.masterMaxNits * 10000.0))) + ","
        + QByteArray::number(qlonglong(std::llround(hdr.masterMinNits * 10000.0))) + ")";
}

QByteArray x265HdrParams(const VideoExportSettings& v)
{
    if (!v.wideGamut())
        return {};
    // colorprim / transfer / colormatrix duplicate what libx265.c sets from the context, so
    // the VUI is right even if an FFmpeg version stops copying them.
    QByteArray p = "colorprim=bt2020:colormatrix=bt2020nc";
    switch (v.gamma) {
    case GammaTag::Pq:
        p += ":transfer=smpte2084:hdr10=1:repeat-headers=1:master-display="
            + x265MasterDisplay(v.hdr) + ":max-cll=" + QByteArray::number(v.hdr.maxCll) + ","
            + QByteArray::number(v.hdr.maxFall);
        break;
    case GammaTag::Hlg:
        p += ":transfer=arib-std-b67";
        break;
    case GammaTag::Rec709:
        p += ":transfer=bt2020-10";
        break;
    }
    return p;
}

QByteArray svtAv1HdrParams(const VideoExportSettings& v)
{
    if (!v.wideGamut())
        return {};
    // CICP numbers: primaries 9 = BT.2020, transfer 16 = PQ / 18 = HLG / 14 = BT.2020 10-bit,
    // matrix 9 = BT.2020 non-constant luminance; color-range 0 = studio.
    const bool hdr2020 = v.hdr.primaries == MasterPrimaries::Rec2020;
    QByteArray p = "color-primaries=9:matrix-coefficients=9:color-range=0";
    switch (v.gamma) {
    case GammaTag::Pq:
        p += ":transfer-characteristics=16";
        p += hdr2020 ? ":mastering-display=G(0.1700,0.7970)B(0.1310,0.0460)R(0.7080,0.2920)"
                       "WP(0.3127,0.3290)L("
                     : ":mastering-display=G(0.2650,0.6900)B(0.1500,0.0600)R(0.6800,0.3200)"
                       "WP(0.3127,0.3290)L("
            + QByteArray::number(v.hdr.masterMaxNits, 'f', 4) + ","
            + QByteArray::number(v.hdr.masterMinNits, 'f', 4) + ")";
        p += ":content-light=" + QByteArray::number(v.hdr.maxCll) + ","
            + QByteArray::number(v.hdr.maxFall);
        break;
    case GammaTag::Hlg:
        p += ":transfer-characteristics=18";
        break;
    case GammaTag::Rec709:
        p += ":transfer-characteristics=14";
        break;
    }
    return p;
}

} // namespace kk::exporter
