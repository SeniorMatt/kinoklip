// SPDX-License-Identifier: GPL-3.0-or-later
// HdrToSdr — HLG / PQ / BT.2020 → SDR BT.709 (see HdrToSdr.h for the steps).
#include "media/HdrToSdr.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <vector>

namespace kk::media {

namespace {

// BT.2100 HLG inverse OETF constants.
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892; // 1 - 4a
constexpr double kHlgC = 0.55991073; // 0.5 - a ln(4a)
// BT.2100 PQ constants.
constexpr double kPqM1 = 0.1593017578125;
constexpr double kPqM2 = 78.84375;
constexpr double kPqC1 = 0.8359375;
constexpr double kPqC2 = 18.8515625;
constexpr double kPqC3 = 18.6875;
/// HDR reference white (BT.2408), cd/m2; shown as SDR 1.0 before the highlight roll-off.
constexpr double kReferenceWhiteNits = 203.0;
/// HLG nominal peak display luminance (cd/m2) of the BT.2100 OOTF.
constexpr double kHlgPeakNits = 1000.0;
constexpr double kHlgSystemGamma = 1.2;
/// Linear-light values up to this level are kept; above it highlights are compressed.
constexpr double kKnee = 0.8;
constexpr double kDisplayGamma = 2.4;
/// BT.2020 → BT.709 primaries in linear light (row-major).
constexpr double kBt2020To709[3][3] = {{1.6604910, -0.5876411, -0.0728499},
    {-0.1245505, 1.1328999, -0.0083494}, {-0.0181508, -0.1005789, 1.1187297}};
/// BT.2020 luminance weights (the HLG OOTF works on scene luminance).
constexpr double kLumaR = 0.2627, kLumaG = 0.6780, kLumaB = 0.0593;

double hlgInverseOetf(double e)
{
    e = std::clamp(e, 0.0, 1.0);
    return e <= 0.5 ? e * e / 3.0 : (std::exp((e - kHlgC) / kHlgA) + kHlgB) / 12.0;
}

double pqEotfNits(double e)
{
    const double p = std::pow(std::clamp(e, 0.0, 1.0), 1.0 / kPqM2);
    return 10000.0 * std::pow(std::max(p - kPqC1, 0.0) / (kPqC2 - kPqC3 * p), 1.0 / kPqM1);
}

/// Linear display light of the HLG OOTF at reference white (75 % signal), in OOTF units.
double hlgReferenceWhite()
{
    static const double white = std::pow(hlgInverseOetf(0.75), kHlgSystemGamma);
    return white;
}

double toneMap(double x)
{
    if (x <= kKnee)
        return x;
    const double t = (x - kKnee) / (1.0 - kKnee);
    return kKnee + (1.0 - kKnee) * t / (1.0 + t);
}

double encode(double linear)
{
    return std::pow(std::clamp(linear, 0.0, 1.0), 1.0 / kDisplayGamma);
}

/// Step 1 + 2 for one channel (HLG: scene light only; the OOTF needs all three channels).
double linearise(double e, HdrMapping mapping)
{
    switch (mapping) {
    case HdrMapping::Hlg:
        return hlgInverseOetf(e);
    case HdrMapping::Pq:
        return pqEotfNits(e) / kReferenceWhiteNits;
    case HdrMapping::WideGamut:
        return std::pow(std::clamp(e, 0.0, 1.0), kDisplayGamma);
    case HdrMapping::None:
        break;
    }
    return e;
}

/// HLG OOTF gain for scene luminance @p ys, normalised to reference white = 1.0.
double hlgGain(double ys)
{
    return ys > 0 ? std::pow(ys, kHlgSystemGamma - 1.0) / hlgReferenceWhite() : 0.0;
}

/// Lookup tables of mapImageToSdr() for one mapping.
struct Tables
{
    static constexpr int kGainSize = 4096; ///< HLG gain over scene luminance 0..1
    static constexpr int kEncodeSteps = 1024; ///< encode entries per unit of linear light
    static constexpr int kEncodeMax = 16; ///< linear light above this clips (tone mapped ~1)
    float decode[256];
    std::vector<float> gain; ///< HLG only
    std::vector<uint8_t> out; ///< tone map + encode, index = linear * kEncodeSteps

    explicit Tables(HdrMapping mapping)
    {
        for (int i = 0; i < 256; ++i)
            decode[i] = float(linearise(i / 255.0, mapping));
        if (mapping == HdrMapping::Hlg) {
            gain.resize(kGainSize + 1);
            for (int i = 0; i <= kGainSize; ++i)
                gain[size_t(i)] = float(hlgGain(double(i) / kGainSize));
        }
        const bool toneMapped = mapping != HdrMapping::WideGamut;
        out.resize(size_t(kEncodeMax * kEncodeSteps) + 1);
        for (size_t i = 0; i < out.size(); ++i) {
            const double x = double(i) / kEncodeSteps;
            out[i] = uint8_t(std::lround(255.0 * encode(toneMapped ? toneMap(x) : x)));
        }
    }

    uint8_t encodeByte(float linear) const
    {
        const float scaled = std::clamp(linear, 0.0f, float(kEncodeMax)) * kEncodeSteps;
        return out[size_t(scaled + 0.5f)];
    }
};

const Tables& tablesFor(HdrMapping mapping)
{
    static const Tables hlg(HdrMapping::Hlg);
    static const Tables pq(HdrMapping::Pq);
    static const Tables wide(HdrMapping::WideGamut);
    return mapping == HdrMapping::Hlg ? hlg : mapping == HdrMapping::Pq ? pq : wide;
}

const char* const kGlsl = R"(
vec3 hdrToSdr(vec3 c, int mapping)
{
    if (mapping == 0)
        return c;
    c = clamp(c, 0.0, 1.0);
    vec3 lin;
    if (mapping == 1) { // HLG: inverse OETF, OOTF (gamma 1.2), reference white = 1
        vec3 lo = c * c / 3.0;
        vec3 hi = (exp((c - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
        lin = mix(lo, hi, step(0.5, c));
        float ys = dot(lin, vec3(0.2627, 0.6780, 0.0593));
        lin *= (ys > 0.0 ? pow(ys, 0.2) : 0.0) / HLG_REFERENCE_WHITE;
    } else if (mapping == 2) { // PQ: EOTF in cd/m2 / 203
        vec3 p = pow(c, vec3(1.0 / 78.84375));
        lin = pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(1.0 / 0.1593017578125))
            * (10000.0 / 203.0);
    } else { // BT.2020 SDR
        lin = pow(c, vec3(2.4));
    }
    // BT.2020 -> BT.709 (GLSL matrices are column-major: these are the rows' transpose).
    lin = max(mat3(1.6604910, -0.1245505, -0.0181508, -0.5876411, 1.1328999, -0.1005789,
                  -0.0728499, -0.0083494, 1.1187297) * lin, 0.0);
    if (mapping != 3) {
        vec3 t = max(lin - 0.8, 0.0) / 0.2;
        lin = mix(lin, 0.8 + 0.2 * t / (1.0 + t), step(0.8, lin));
    }
    return pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.4));
}
)";

} // namespace

HdrMapping hdrMappingOf(const AVFrame* frame)
{
    if (!frame)
        return HdrMapping::None;
    switch (frame->color_trc) {
    case AVCOL_TRC_ARIB_STD_B67:
        return HdrMapping::Hlg;
    case AVCOL_TRC_SMPTE2084:
        return HdrMapping::Pq;
    default:
        break;
    }
    return frame->color_primaries == AVCOL_PRI_BT2020 ? HdrMapping::WideGamut : HdrMapping::None;
}

HdrMapping hdrMappingOf(const VideoFrame& frame)
{
    if (frame.format == PixelFormat::Bgra)
        return HdrMapping::None; // already converted (stills, rare layouts via FrameConverter)
    switch (frame.transfer) {
    case ColorTransfer::Hlg:
        return HdrMapping::Hlg;
    case ColorTransfer::Pq:
        return HdrMapping::Pq;
    case ColorTransfer::Sdr:
        break;
    }
    return frame.avFrame && frame.avFrame->color_primaries == AVCOL_PRI_BT2020
        ? HdrMapping::WideGamut
        : HdrMapping::None;
}

std::array<float, 3> mapToSdr(std::array<float, 3> rgb, HdrMapping mapping)
{
    if (mapping == HdrMapping::None)
        return rgb;
    double lin[3];
    for (int i = 0; i < 3; ++i)
        lin[i] = linearise(rgb[size_t(i)], mapping);
    if (mapping == HdrMapping::Hlg) {
        const double gain = hlgGain(kLumaR * lin[0] + kLumaG * lin[1] + kLumaB * lin[2]);
        for (double& v : lin)
            v *= gain;
    }
    std::array<float, 3> out {};
    for (int row = 0; row < 3; ++row) {
        double v = 0;
        for (int col = 0; col < 3; ++col)
            v += kBt2020To709[row][col] * lin[col];
        v = std::max(v, 0.0);
        out[size_t(row)] = float(encode(mapping == HdrMapping::WideGamut ? v : toneMap(v)));
    }
    return out;
}

void mapImageToSdr(QImage* image, HdrMapping mapping)
{
    if (!image || image->isNull() || mapping == HdrMapping::None)
        return;
    if (image->format() != QImage::Format_ARGB32
        && image->format() != QImage::Format_ARGB32_Premultiplied
        && image->format() != QImage::Format_RGB32)
        return;
    const Tables& t = tablesFor(mapping);
    const bool hlg = mapping == HdrMapping::Hlg;
    for (int y = 0; y < image->height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image->scanLine(y));
        for (int x = 0; x < image->width(); ++x) {
            const QRgb p = row[x];
            float r = t.decode[qRed(p)], g = t.decode[qGreen(p)], b = t.decode[qBlue(p)];
            if (hlg) {
                const float ys
                    = std::min(1.0f, float(kLumaR) * r + float(kLumaG) * g + float(kLumaB) * b);
                const float gain = t.gain[size_t(ys * Tables::kGainSize + 0.5f)];
                r *= gain;
                g *= gain;
                b *= gain;
            }
            const float r709 = float(kBt2020To709[0][0]) * r + float(kBt2020To709[0][1]) * g
                + float(kBt2020To709[0][2]) * b;
            const float g709 = float(kBt2020To709[1][0]) * r + float(kBt2020To709[1][1]) * g
                + float(kBt2020To709[1][2]) * b;
            const float b709 = float(kBt2020To709[2][0]) * r + float(kBt2020To709[2][1]) * g
                + float(kBt2020To709[2][2]) * b;
            row[x] = qRgba(t.encodeByte(r709), t.encodeByte(g709), t.encodeByte(b709), qAlpha(p));
        }
    }
}

const char* hdrToSdrGlsl()
{
    static const QByteArray source = [] {
        QByteArray s(kGlsl);
        s.replace("HLG_REFERENCE_WHITE", QByteArray::number(hlgReferenceWhite(), 'f', 8));
        return s;
    }();
    return source.constData();
}

} // namespace kk::media
