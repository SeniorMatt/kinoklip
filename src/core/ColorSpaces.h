// SPDX-License-Identifier: GPL-3.0-or-later
// ColorSpaces — the colour science behind the Resolve FX Color Space Transform, ACES Transform,
// Gamut Mapping, Chromatic Adaptation and Color Compressor (docs/behavior-spec.md section 205).
// Everything is computed from published numbers: primaries and white points of the gamuts,
// the transfer functions of the standards and camera makers, the Bradford / CAT02 / von Kries
// cone matrices, and the ACES RRT + ODT as the published analytic fit (Hill / Narkowicz, MIT).
// No matrix is typed in: RGB -> XYZ comes from the primaries, so the tests can check the
// results against known values (sRGB -> ACEScg and so on). The GLSL twin of the per-pixel
// functions is in render/GlColorManagement.cpp; keep both in step.
//
// Light units: scene-linear 1.0 is the reference white (18 % grey = 0.18) for scene curves;
// for the display-referred PQ and HLG curves 1.0 is 100 nits (SDR reference white).
#pragma once

#include <QString>
#include <QStringList>

#include <array>

namespace kk::cm {

using Mat3 = std::array<double, 9>; ///< row-major
using Vec3 = std::array<double, 3>;

struct Chromaticity
{
    double x = 0.3127;
    double y = 0.3290;
};

enum class Gamut {
    Rec709 = 0, ///< also sRGB
    Rec2020,
    P3D65,
    P3Dci,
    P3D60,
    AcesAp0,
    AcesAp1, ///< ACEScg
    AdobeRgb,
    DavinciWide,
    AlexaWide,
    SGamut3,
    SGamut3Cine,
    VGamut,
    RedWide,
    Count
};

enum class Transfer {
    Linear = 0,
    Gamma22,
    Gamma24,
    Gamma26,
    Srgb,
    Rec709,
    Pq,
    Hlg,
    AcesCc,
    AcesCct,
    DavinciIntermediate,
    LogC3,
    SLog3,
    VLog,
    Log3G10,
    Count
};

enum class WhitePoint { D50 = 0, D55, D60, D65, Dci, D75, IlluminantA, IlluminantE, Custom, Count };

enum class Cat { Bradford = 0, Cat02, VonKries };

enum class ToneMap { None = 0, Simple, Luminance, Davinci };
enum class GamutMap { None = 0, Saturation, Compression };

/// One row of a choice list: the stored id and the English label (the effect catalogue
/// translates the label).
struct Entry
{
    const char* id;
    const char* label;
};

[[nodiscard]] const Entry& gamutEntry(Gamut g);
[[nodiscard]] const Entry& transferEntry(Transfer t);
[[nodiscard]] const Entry& whitePointEntry(WhitePoint w);
[[nodiscard]] Gamut gamutFromId(const QString& id, Gamut fallback);
[[nodiscard]] Transfer transferFromId(const QString& id, Transfer fallback);
[[nodiscard]] WhitePoint whitePointFromId(const QString& id, WhitePoint fallback);

/// One entry of the ACES Transform's input / output lists: a colour encoding. `display` entries
/// are display encodings: as an output they come with the RRT + ODT, as an input of an
/// inverted transform they are undone by the inverse.
struct AcesEntry
{
    const char* id;
    const char* label;
    Gamut gamut;
    Transfer transfer;
    bool display;
};
[[nodiscard]] int acesEntryCount();
[[nodiscard]] const AcesEntry& acesEntry(int i);
[[nodiscard]] const AcesEntry* acesEntryFromId(const QString& id);

// ---- gamuts and white points ----

[[nodiscard]] Chromaticity whiteOf(Gamut g);
[[nodiscard]] Chromaticity whiteOf(WhitePoint w, Chromaticity custom = {});
/// XYZ of a white with luminance 1.
[[nodiscard]] Vec3 xyzOf(Chromaticity c);
/// Linear RGB (with the gamut's own white) -> CIE XYZ, built from the primaries.
[[nodiscard]] Mat3 rgbToXyz(Gamut g);
[[nodiscard]] Mat3 xyzToRgb(Gamut g);
[[nodiscard]] Mat3 multiply(const Mat3& a, const Mat3& b);
[[nodiscard]] Mat3 inverse(const Mat3& m);
[[nodiscard]] Vec3 mulVec(const Mat3& m, const Vec3& v);
/// The cone-response matrix of @p cat (XYZ -> cone space).
[[nodiscard]] Mat3 coneMatrix(Cat cat);
/// XYZ -> XYZ matrix that moves the white @p from to the white @p to.
[[nodiscard]] Mat3 adaptation(Cat cat, Chromaticity from, Chromaticity to);
/// Linear RGB of @p from -> linear RGB of @p to; the whites are adapted with @p cat when
/// @p adapt is true, otherwise XYZ is passed through as it is.
[[nodiscard]] Mat3 gamutMatrix(Gamut from, Gamut to, Cat cat, bool adapt);
/// The luminance (Y) row of RGB -> XYZ.
[[nodiscard]] Vec3 lumaWeights(Gamut g);

// ---- transfer functions (scalar, per channel) ----

[[nodiscard]] float decode(Transfer t, float encoded); ///< encoded signal -> linear light
[[nodiscard]] float encode(Transfer t, float linear); ///< linear light -> encoded signal
[[nodiscard]] bool isDisplayReferred(Transfer t); ///< PQ and HLG

// ---- per-pixel pipeline ----

/// The resolved colour pipeline all five effects share. Per pixel: decode the input curve,
/// optional HLG forward OOTF, matrix m, ACES RRT + ODT fit (rrt 1) or its inverse (rrt 2),
/// matrix m2, highlight compression, tone mapping, gamut mapping, optional HLG inverse OOTF,
/// matrix m3, encode the output curve, mix with the input by @c amount.
struct CstParams
{
    float m[9] {1, 0, 0, 0, 1, 0, 0, 0, 1}; ///< row-major linear RGB(in) -> working RGB
    float m2[9] {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float m3[9] {1, 0, 0, 0, 1, 0, 0, 0, 1};
    int inTf = 0, outTf = 0, tone = 0, gmap = 0;
    int rrt = 0; ///< 0 none, 1 RRT + ODT (m must end in ACEScg), 2 inverse
    float hiThr = 0.8f, hiLim = 0.f, hiPwr = 1.2f; ///< per-channel highlight compression, hiLim 0 = off
    float peakIn = 10.f, peakOut = 1.f; ///< max luminance in units of 100 nits
    bool fwdOotf = false, invOotf = false;
    float lumaIn[3] {0.2126f, 0.7152f, 0.0722f};
    float lumaOut[3] {0.2126f, 0.7152f, 0.0722f};
    float gmThr = 0.8f, gmLim = 1.2f, gmPwr = 1.2f;
    float amount = 1.f; ///< 0..1 mix with the unchanged colour
};

/// Color Space Transform.
[[nodiscard]] CstParams makeCst(Gamut inGamut, Transfer inTf, Gamut outGamut, Transfer outTf,
    ToneMap tone, GamutMap gmap, bool fwdOotf, bool invOotf, bool adapt, double inNits,
    double outNits, Cat cat = Cat::Bradford);
void cstApply(const CstParams& p, const float in[3], float out[3]);

/// ACES Transform: @p input into ACES and out through @p output; @p invert treats a display
/// @p input as the output of an ACES output transform and undoes it.
[[nodiscard]] CstParams makeAces(const AcesEntry& input, const AcesEntry& output, bool invert);
/// Gamut Mapping: the picture's @p gamut and @p tf, compressed into @p target.
[[nodiscard]] CstParams makeGamutMapping(Gamut gamut, Transfer tf, Gamut target, GamutMap mode,
    double threshold, double limit, double power, double amount);
/// Chromatic Adaptation of a picture in @p gamut / @p tf from white @p from to white @p to.
[[nodiscard]] CstParams makeAdaptation(
    Gamut gamut, Transfer tf, Cat cat, Chromaticity from, Chromaticity to, double amount);
/// Color Compressor.
[[nodiscard]] CstParams makeCompressor(Gamut gamut, Transfer tf, double hiThr, double hiLim,
    double satThr, double satLim, double amount);

/// Rec. 2100 HLG OOTF exponent for a display of @p nits peak.
[[nodiscard]] float hlgGamma(float nits);

/// ACES Reference Rendering Transform and Output Device Transform as one analytic fit. Input
/// and output are linear ACES AP1 RGB (ACEScg); the output is display-linear in the AP1
/// primaries and not yet clipped.
void rrtOdtFit(const float acesCg[3], float out[3]);
/// The matrices of rrtOdtFit() (row-major): ACEScg -> the space of the curve and back, and their
/// inverses (the GPU twin needs them).
void fitMatrices(float toFit[9], float fromFit[9], float toFitInv[9], float fromFitInv[9]);
/// Inverse of rrtOdtFit().
void rrtOdtFitInverse(const float displayAp1[3], float out[3]);

/// Soft compression of a distance in 0..1+: unchanged below @p thr, and @p lim maps to 1.
[[nodiscard]] float compress(float x, float thr, float lim, float power);
/// Pulls negative and over-saturated linear RGB back toward the grey axis by compressing each
/// channel's distance from max(r,g,b).
void compressGamut(float rgb[3], float thr, float lim, float power);

} // namespace kk::cm
