// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ColorMatch.h"

#include "core/Grade.h"
#include "core/GradeMath.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace kk::colormatch {

namespace {

namespace gk = gradekeys;
using Mat3 = std::array<double, 9>; ///< row-major
using Vec3 = std::array<double, 3>;

constexpr Mat3 kIdentity = {1, 0, 0, 0, 1, 0, 0, 0, 1};

Vec3 mul(const Mat3& m, const Vec3& v)
{
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
        m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Mat3 mul(const Mat3& a, const Mat3& b)
{
    Mat3 r {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k)
                r[size_t(i * 3 + j)] += a[size_t(i * 3 + k)] * b[size_t(k * 3 + j)];
        }
    }
    return r;
}

Mat3 inverse(const Mat3& m)
{
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7],
                 i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12)
        return kIdentity;
    const double s = 1.0 / det;
    return {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, (f * g - d * i) * s,
        (a * i - c * g) * s, (c * d - a * f) * s, (d * h - e * g) * s, (b * g - a * h) * s,
        (a * e - b * d) * s};
}

Vec3 xyToXyz(double x, double y)
{
    return {x / y, 1.0, (1.0 - x - y) / y};
}

struct Primaries
{
    double rx, ry, gx, gy, bx, by;
};

Primaries primariesOf(ColorSpace space)
{
    switch (space) {
    case ColorSpace::DciP3:
        return {0.680, 0.320, 0.265, 0.690, 0.150, 0.060};
    case ColorSpace::Rec2020:
        return {0.708, 0.292, 0.170, 0.797, 0.131, 0.046};
    case ColorSpace::Rec709:
        break;
    }
    return {0.640, 0.330, 0.300, 0.600, 0.150, 0.060};
}

Vec3 whiteXyz(WhitePoint white)
{
    switch (white) {
    case WhitePoint::D60:
        return xyToXyz(0.32168, 0.33767);
    case WhitePoint::D50:
        return xyToXyz(0.3457, 0.3585);
    case WhitePoint::Dci:
        return xyToXyz(0.314, 0.351);
    case WhitePoint::D65:
        break;
    }
    return xyToXyz(0.3127, 0.3290);
}

/// Linear RGB -> XYZ of @p space with white @p white (white = RGB 1,1,1).
Mat3 rgbToXyz(ColorSpace space, const Vec3& white)
{
    const Primaries p = primariesOf(space);
    const Vec3 r = xyToXyz(p.rx, p.ry), g = xyToXyz(p.gx, p.gy), b = xyToXyz(p.bx, p.by);
    const Mat3 cols = {r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const Vec3 s = mul(inverse(cols), white);
    return {r[0] * s[0], g[0] * s[1], b[0] * s[2], r[1] * s[0], g[1] * s[1], b[1] * s[2],
        r[2] * s[0], g[2] * s[1], b[2] * s[2]};
}

/// Bradford adaptation matrix from white @p from to white @p to.
Mat3 bradford(const Vec3& from, const Vec3& to)
{
    const Mat3 m = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
    const Vec3 a = mul(m, from), b = mul(m, to);
    const Mat3 scale = {b[0] / a[0], 0, 0, 0, b[1] / a[1], 0, 0, 0, b[2] / a[2]};
    return mul(inverse(m), mul(scale, m));
}

/// The published sRGB values (8 bit) of the ColorChecker Classic, row by row.
constexpr int kClassicSrgb[kChartPatches][3] = {{115, 82, 68}, {194, 150, 130}, {98, 122, 157},
    {87, 108, 67}, {133, 128, 177}, {103, 189, 170}, {214, 126, 44}, {80, 91, 166},
    {193, 90, 99}, {94, 60, 108}, {157, 188, 64}, {224, 163, 46}, {56, 61, 150}, {70, 148, 73},
    {175, 54, 60}, {231, 199, 31}, {187, 86, 149}, {8, 133, 161}, {243, 243, 242},
    {200, 200, 200}, {160, 160, 160}, {122, 122, 121}, {85, 85, 85}, {52, 52, 52}};

/// Rough power of a gamma (the starting point of the tone fit).
double effectiveGamma(Gamma g)
{
    switch (g) {
    case Gamma::Linear:
        return 1.0;
    case Gamma::Gamma22:
    case Gamma::Srgb:
        return 2.2;
    case Gamma::Gamma24:
        return 2.4;
    case Gamma::Gamma26:
        return 2.6;
    case Gamma::Rec709:
        return 2.0;
    }
    return 2.2;
}

/// Solves the n x n system A x = b in place (Gaussian elimination, partial pivoting).
bool solveLinear(std::vector<double>& a, std::vector<double>& b, int n)
{
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r) {
            if (std::abs(a[size_t(r * n + c)]) > std::abs(a[size_t(piv * n + c)]))
                piv = r;
        }
        if (std::abs(a[size_t(piv * n + c)]) < 1e-14)
            return false;
        if (piv != c) {
            for (int k = 0; k < n; ++k)
                std::swap(a[size_t(c * n + k)], a[size_t(piv * n + k)]);
            std::swap(b[size_t(c)], b[size_t(piv)]);
        }
        for (int r = c + 1; r < n; ++r) {
            const double f = a[size_t(r * n + c)] / a[size_t(c * n + c)];
            for (int k = c; k < n; ++k)
                a[size_t(r * n + k)] -= f * a[size_t(c * n + k)];
            b[size_t(r)] -= f * b[size_t(c)];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[size_t(r)];
        for (int k = r + 1; k < n; ++k)
            s -= a[size_t(r * n + k)] * b[size_t(k)];
        b[size_t(r)] = s / a[size_t(r * n + r)];
    }
    return true;
}

constexpr double kLiftLimit = 0.5, kGammaLimit = 1.5, kGainMin = 0.05, kGainMax = 6.0;

/// Fits lift, gamma and gain of one channel to the neutral patches (@p m measured, @p t target).
void fitTone(const double* m, const double* t, int n, double g0, float lift[1], float gamma[1],
    float gain[1])
{
    auto model = [](double x, const double p[3]) {
        return double(grademath::liftGammaGain(float(x), float(p[0]), float(p[1]), float(p[2])));
    };
    auto cost = [&](const double p[3]) {
        double s = 0;
        for (int i = 0; i < n; ++i) {
            const double r = model(m[i], p) - t[i];
            s += r * r;
        }
        return s;
    };
    const double e0 = std::pow(2.0, -2.0 * g0);
    // Start: gain that puts the brightest patch on its target.
    const double mw = std::max(m[0], 1e-3), tw = std::max(t[0], 1e-3);
    double p[3] = {0.0, std::clamp(g0, -kGammaLimit, kGammaLimit),
        std::clamp(std::pow(tw, 1.0 / e0) / mw, kGainMin, kGainMax)};
    double lambda = 1e-3;
    double best = cost(p);
    for (int iter = 0; iter < 80; ++iter) {
        double jac[3][16] {};
        double res[16] {};
        for (int i = 0; i < n; ++i)
            res[i] = model(m[i], p) - t[i];
        for (int k = 0; k < 3; ++k) {
            double q[3] = {p[0], p[1], p[2]};
            const double h = 1e-4;
            q[k] += h;
            for (int i = 0; i < n; ++i)
                jac[k][i] = (model(m[i], q) - model(m[i], p)) / h;
        }
        std::vector<double> a(9), b(3);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                double s = 0;
                for (int i = 0; i < n; ++i)
                    s += jac[r][i] * jac[c][i];
                a[size_t(r * 3 + c)] = s + (r == c ? lambda * (s + 1e-9) : 0.0);
            }
            double s = 0;
            for (int i = 0; i < n; ++i)
                s -= jac[r][i] * res[i];
            b[size_t(r)] = s;
        }
        if (!solveLinear(a, b, 3)) {
            lambda *= 10;
            continue;
        }
        double q[3] = {std::clamp(p[0] + b[0], -kLiftLimit, kLiftLimit),
            std::clamp(p[1] + b[1], -kGammaLimit, kGammaLimit),
            std::clamp(p[2] + b[2], kGainMin, kGainMax)};
        const double c = cost(q);
        if (c < best) {
            const double change = std::abs(best - c);
            best = c;
            std::copy(q, q + 3, p);
            lambda = std::max(lambda * 0.3, 1e-9);
            if (change < 1e-14)
                break;
        } else {
            lambda *= 5;
            if (lambda > 1e8)
                break;
        }
    }
    lift[0] = float(p[0]);
    gamma[0] = float(p[1]);
    gain[0] = float(p[2]);
}

} // namespace

// ---- Shot Match ----

ShotStats shotStats(const float* rgb, size_t pixels)
{
    ShotStats s;
    if (pixels < 16)
        return s;
    constexpr int kBins = 1024;
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<uint32_t> hist(kBins, 0);
        for (size_t i = 0; i < pixels; ++i) {
            const float v = std::clamp(rgb[i * 3 + size_t(ch)], 0.0f, 1.0f);
            ++hist[size_t(std::min(int(v * kBins), kBins - 1))];
        }
        auto percentile = [&](double fraction) {
            const double target = fraction * double(pixels);
            double seen = 0.0;
            for (int b = 0; b < kBins; ++b) {
                seen += hist[size_t(b)];
                if (seen >= target)
                    return (float(b) + 0.5f) / float(kBins);
            }
            return 1.0f;
        };
        s.low[ch] = percentile(0.05);
        s.mid[ch] = percentile(0.5);
        s.high[ch] = percentile(0.95);
    }
    s.valid = true;
    return s;
}

ShotMatch shotMatch(const ShotStats& source, const ShotStats& target, double strength)
{
    ShotMatch m;
    if (!source.valid || !target.valid)
        return m;
    const float k = float(std::clamp(strength, 0.0, 1.0));
    for (int ch = 0; ch < 3; ++ch) {
        const float srcSpread = std::max(source.high[ch] - source.low[ch], 0.02f);
        const float dstSpread = std::max(target.high[ch] - target.low[ch], 0.02f);
        const float full = std::clamp(dstSpread / srcSpread, kShotGainMin, kShotGainMax);
        const float gain = 1.0f + (full - 1.0f) * k;
        const float wantedMid = source.mid[ch] + (target.mid[ch] - source.mid[ch]) * k;
        m.gain[ch] = gain;
        m.offset[ch] = std::clamp(
            wantedMid / gain - source.mid[ch], -kShotOffsetLimit, kShotOffsetLimit);
    }
    m.valid = true;
    return m;
}

ShotMatch composeShotMatch(const ShotMatch& first, const ShotMatch& second)
{
    ShotMatch r;
    for (int ch = 0; ch < 3; ++ch) {
        r.gain[ch] = first.gain[ch] * second.gain[ch];
        r.offset[ch] = first.offset[ch] + second.offset[ch] / first.gain[ch];
    }
    r.valid = first.valid || second.valid;
    return r;
}

QVector<std::pair<QString, double>> shotMatchParams(const ShotMatch& match)
{
    QVector<std::pair<QString, double>> v;
    for (int ch = 0; ch < 3; ++ch) {
        v.append({gk::wheelKey(gk::Wheel::Gain, ch), double(match.gain[ch])});
        v.append({gk::wheelKey(gk::Wheel::Offset, ch), double(match.offset[ch])});
    }
    return v;
}

quint64 applyShotMatch(ClipGrade& grade, const ShotMatch& match)
{
    if (!match.valid)
        return 0;
    GradeNode* node = nullptr;
    ShotMatch total = match;
    if (grade.isEmpty()) {
        node = grade.node(gradeops::initialize(grade));
    } else {
        GradeNode* out = grade.node(grade.output);
        if (!out || out->type != GradeNodeType::Corrector)
            return 0;
        if (out->label == kShotMatchLabel) {
            node = out;
            ShotMatch old;
            old.valid = true;
            for (int ch = 0; ch < 3; ++ch) {
                old.gain[ch] = float(out->value(gk::wheelKey(gk::Wheel::Gain, ch), 0));
                old.offset[ch] = float(out->value(gk::wheelKey(gk::Wheel::Offset, ch), 0));
            }
            total = composeShotMatch(old, match);
        } else {
            node = grade.node(gradeops::addSerialNode(grade, grade.output));
        }
    }
    if (!node)
        return 0;
    node->label = kShotMatchLabel;
    for (const auto& [key, value] : shotMatchParams(total))
        node->params.setBase(key, value);
    return node->id;
}

void applyChartToNode(GradeNode& node, const ChartMatch& match)
{
    for (int ch = 0; ch < 3; ++ch)
        node.params.setBase(gk::wheelKey(gk::Wheel::Offset, ch), 0.0);
    node.params.setBase(gk::LiftY, 0.0);
    node.params.setBase(gk::GammaY, 0.0);
    node.params.setBase(gk::GainY, 1.0);
    node.params.setBase(gk::OffsetY, 0.0);
    node.params.setBase(gk::MixerMono, 0.0);
    for (const auto& [key, value] : chartMatchParams(match))
        node.params.setBase(key, value);
}

// ---- Names and ids ----

QString chartName(ChartType type)
{
    return type == ChartType::ColorCheckerPassport
        ? QCoreApplication::translate("kk::colormatch", "X-Rite ColorChecker Passport Video")
        : QCoreApplication::translate("kk::colormatch", "X-Rite ColorChecker Classic 24");
}

QString gammaName(Gamma gamma)
{
    switch (gamma) {
    case Gamma::Linear:
        return QCoreApplication::translate("kk::colormatch", "Linear");
    case Gamma::Gamma22:
        return QCoreApplication::translate("kk::colormatch", "Gamma 2.2");
    case Gamma::Gamma24:
        return QCoreApplication::translate("kk::colormatch", "Gamma 2.4");
    case Gamma::Gamma26:
        return QCoreApplication::translate("kk::colormatch", "Gamma 2.6");
    case Gamma::Srgb:
        return QCoreApplication::translate("kk::colormatch", "sRGB");
    case Gamma::Rec709:
        return QCoreApplication::translate("kk::colormatch", "Rec.709");
    }
    return {};
}

QString colorSpaceName(ColorSpace space)
{
    switch (space) {
    case ColorSpace::Rec709:
        return QCoreApplication::translate("kk::colormatch", "Rec.709");
    case ColorSpace::DciP3:
        return QCoreApplication::translate("kk::colormatch", "DCI-P3");
    case ColorSpace::Rec2020:
        return QCoreApplication::translate("kk::colormatch", "Rec.2020");
    }
    return {};
}

QString whitePointName(WhitePoint white)
{
    switch (white) {
    case WhitePoint::D65:
        return QCoreApplication::translate("kk::colormatch", "D65 (6504 K)");
    case WhitePoint::D60:
        return QCoreApplication::translate("kk::colormatch", "D60 (6000 K)");
    case WhitePoint::D50:
        return QCoreApplication::translate("kk::colormatch", "D50 (5003 K)");
    case WhitePoint::Dci:
        return QCoreApplication::translate("kk::colormatch", "DCI (6300 K)");
    }
    return {};
}

QString chartId(ChartType type)
{
    return type == ChartType::ColorCheckerPassport ? QStringLiteral("passport")
                                                   : QStringLiteral("classic24");
}

ChartType chartFromId(const QString& id)
{
    return id == QLatin1String("passport") ? ChartType::ColorCheckerPassport
                                           : ChartType::ColorCheckerClassic;
}

QString gammaId(Gamma gamma)
{
    static const char* ids[] = {"linear", "g22", "g24", "g26", "srgb", "rec709"};
    return QLatin1String(ids[int(gamma)]);
}

Gamma gammaFromId(const QString& id)
{
    for (int i = 0; i <= int(Gamma::Rec709); ++i) {
        if (gammaId(Gamma(i)) == id)
            return Gamma(i);
    }
    return Gamma::Rec709;
}

QString colorSpaceId(ColorSpace space)
{
    static const char* ids[] = {"rec709", "p3", "rec2020"};
    return QLatin1String(ids[int(space)]);
}

ColorSpace colorSpaceFromId(const QString& id)
{
    for (int i = 0; i <= int(ColorSpace::Rec2020); ++i) {
        if (colorSpaceId(ColorSpace(i)) == id)
            return ColorSpace(i);
    }
    return ColorSpace::Rec709;
}

QString whitePointId(WhitePoint white)
{
    static const char* ids[] = {"d65", "d60", "d50", "dci"};
    return QLatin1String(ids[int(white)]);
}

WhitePoint whitePointFromId(const QString& id)
{
    for (int i = 0; i <= int(WhitePoint::Dci); ++i) {
        if (whitePointId(WhitePoint(i)) == id)
            return WhitePoint(i);
    }
    return WhitePoint::D65;
}

// ---- Gamma ----

float gammaEncode(Gamma gamma, float linear)
{
    const float l = std::max(linear, 0.0f);
    switch (gamma) {
    case Gamma::Linear:
        return l;
    case Gamma::Gamma22:
        return std::pow(l, 1.0f / 2.2f);
    case Gamma::Gamma24:
        return std::pow(l, 1.0f / 2.4f);
    case Gamma::Gamma26:
        return std::pow(l, 1.0f / 2.6f);
    case Gamma::Srgb:
        return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    case Gamma::Rec709:
        return l < 0.018f ? 4.5f * l : 1.099f * std::pow(l, 0.45f) - 0.099f;
    }
    return l;
}

float gammaDecode(Gamma gamma, float encoded)
{
    const float v = std::max(encoded, 0.0f);
    switch (gamma) {
    case Gamma::Linear:
        return v;
    case Gamma::Gamma22:
        return std::pow(v, 2.2f);
    case Gamma::Gamma24:
        return std::pow(v, 2.4f);
    case Gamma::Gamma26:
        return std::pow(v, 2.6f);
    case Gamma::Srgb:
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    case Gamma::Rec709:
        return v < 0.081f ? v / 4.5f : std::pow((v + 0.099f) / 1.099f, 1.0f / 0.45f);
    }
    return v;
}

// ---- Chart ----

QString chartPatchName(ChartType, int index)
{
    static const char* names[kChartPatches] = {QT_TRANSLATE_NOOP("kk::colormatch", "Dark Skin"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Light Skin"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Blue Sky"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Foliage"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Blue Flower"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Bluish Green"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Orange"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Purplish Blue"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Moderate Red"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Purple"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Yellow Green"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Orange Yellow"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Blue"), QT_TRANSLATE_NOOP("kk::colormatch", "Green"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Red"), QT_TRANSLATE_NOOP("kk::colormatch", "Yellow"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Magenta"), QT_TRANSLATE_NOOP("kk::colormatch", "Cyan"),
        QT_TRANSLATE_NOOP("kk::colormatch", "White"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Neutral 8"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Neutral 6.5"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Neutral 5"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Neutral 3.5"),
        QT_TRANSLATE_NOOP("kk::colormatch", "Black")};
    if (index < 0 || index >= kChartPatches)
        return {};
    return QCoreApplication::translate("kk::colormatch", names[index]);
}

PatchColors chartTargetColors(ChartType, const ChartTarget& target)
{
    PatchColors out {};
    const Vec3 d65 = whiteXyz(WhitePoint::D65);
    const Vec3 white = whiteXyz(target.white);
    const Mat3 toXyz = rgbToXyz(ColorSpace::Rec709, d65);
    const Mat3 fromXyz = inverse(rgbToXyz(target.space, white));
    const Mat3 adapt = bradford(d65, white);
    const Mat3 total = mul(fromXyz, mul(adapt, toXyz));
    for (int i = 0; i < kChartPatches; ++i) {
        Vec3 lin;
        for (int c = 0; c < 3; ++c)
            lin[size_t(c)] = double(gammaDecode(Gamma::Srgb, float(kClassicSrgb[i][c]) / 255.0f));
        const Vec3 rgb = mul(total, lin);
        for (int c = 0; c < 3; ++c) {
            const float v = float(std::clamp(rgb[size_t(c)], 0.0, 1.0));
            out[size_t(i)][size_t(c)] = gammaEncode(target.targetGamma, v);
        }
    }
    return out;
}

ChartQuad defaultChartQuad(double aspect)
{
    aspect = std::max(aspect, 0.1);
    double w = 0.4;
    double h = w / 1.5 * aspect;
    if (h > 0.8) {
        h = 0.8;
        w = h * 1.5 / aspect;
    }
    ChartQuad q;
    q.corner = {QPointF(0.5 - w / 2, 0.5 - h / 2), QPointF(0.5 + w / 2, 0.5 - h / 2),
        QPointF(0.5 + w / 2, 0.5 + h / 2), QPointF(0.5 - w / 2, 0.5 + h / 2)};
    return q;
}

QPointF chartPoint(const ChartQuad& quad, double u, double v)
{
    // Heckbert's unit square to quadrilateral projection.
    const double x0 = quad.corner[0].x(), y0 = quad.corner[0].y();
    const double x1 = quad.corner[1].x(), y1 = quad.corner[1].y();
    const double x2 = quad.corner[2].x(), y2 = quad.corner[2].y();
    const double x3 = quad.corner[3].x(), y3 = quad.corner[3].y();
    const double dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
    const double dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;
    const double det = dx1 * dy2 - dy1 * dx2;
    double g = 0, h = 0;
    if (std::abs(det) > 1e-12 && (std::abs(dx3) > 1e-12 || std::abs(dy3) > 1e-12)) {
        g = (dx3 * dy2 - dy3 * dx2) / det;
        h = (dx1 * dy3 - dy1 * dx3) / det;
    }
    const double a = x1 - x0 + g * x1, b = x3 - x0 + h * x3;
    const double d = y1 - y0 + g * y1, e = y3 - y0 + h * y3;
    const double den = g * u + h * v + 1.0;
    return {(a * u + b * v + x0) / den, (d * u + e * v + y0) / den};
}

namespace {

bool convex(const ChartQuad& q)
{
    double sign = 0;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = q.corner[size_t(i)], b = q.corner[size_t((i + 1) % 4)],
                      c = q.corner[size_t((i + 2) % 4)];
        const double cross = (b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x());
        if (std::abs(cross) < 1e-9)
            return false;
        if (sign == 0)
            sign = cross;
        else if ((cross > 0) != (sign > 0))
            return false;
    }
    return true;
}

} // namespace

bool measureChart(
    const float* rgb, int width, int height, const ChartQuad& quad, PatchColors& patches)
{
    if (!rgb || width < 2 || height < 2 || !convex(quad))
        return false;
    constexpr int kSamples = 6; // per axis in the central half of each patch
    for (int row = 0; row < kChartRows; ++row) {
        for (int col = 0; col < kChartColumns; ++col) {
            double sum[3] = {0, 0, 0};
            int count = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const double u = (col + 0.25 + 0.5 * (sx + 0.5) / kSamples) / kChartColumns;
                    const double v = (row + 0.25 + 0.5 * (sy + 0.5) / kSamples) / kChartRows;
                    const QPointF p = chartPoint(quad, u, v);
                    const int px = int(std::floor(p.x() * width));
                    const int py = int(std::floor(p.y() * height));
                    if (px < 0 || py < 0 || px >= width || py >= height)
                        return false;
                    const float* s = rgb + (size_t(py) * size_t(width) + size_t(px)) * 3;
                    for (int c = 0; c < 3; ++c)
                        sum[c] += double(s[c]);
                    ++count;
                }
            }
            for (int c = 0; c < 3; ++c)
                patches[size_t(row * kChartColumns + col)][size_t(c)] = float(sum[c] / count);
        }
    }
    return true;
}

void applyChartMatch(const ChartMatch& match, float rgb[3])
{
    grademath::PrimaryValues p;
    for (int ch = 0; ch < 3; ++ch) {
        p.lift[ch] = match.lift[ch];
        p.gamma[ch] = match.gamma[ch];
        p.gain[ch] = match.gain[ch];
    }
    grademath::applyPrimaries(p, rgb);
    grademath::MixerValues m;
    std::copy(match.mixer, match.mixer + 9, m.m);
    grademath::applyMixer(m, rgb);
}

ChartMatch solveChart(const PatchColors& measured, ChartType type, const ChartTarget& target)
{
    ChartMatch r;
    const PatchColors want = chartTargetColors(type, target);
    auto error = [&](const ChartMatch* match) {
        double s = 0;
        for (int i = 0; i < kChartPatches; ++i) {
            float c[3] = {measured[size_t(i)][0], measured[size_t(i)][1], measured[size_t(i)][2]};
            if (match)
                applyChartMatch(*match, c);
            for (int ch = 0; ch < 3; ++ch) {
                const double d = double(c[ch]) - double(want[size_t(i)][size_t(ch)]);
                s += d * d;
            }
        }
        return std::sqrt(s / (kChartPatches * 3));
    };
    r.errorBefore = error(nullptr);

    // The neutral patches must show a usable range in every channel.
    constexpr int kN = kChartPatches - kChartFirstNeutral;
    for (int ch = 0; ch < 3; ++ch) {
        if (measured[kChartFirstNeutral][size_t(ch)] - measured[kChartPatches - 1][size_t(ch)] < 0.05f)
            return r;
    }
    // 1. Tone: lift, gamma and gain of each channel from the neutral row.
    const double g0 = -0.5 * std::log2(effectiveGamma(target.sourceGamma) / effectiveGamma(target.targetGamma));
    for (int ch = 0; ch < 3; ++ch) {
        double m[kN], t[kN];
        for (int i = 0; i < kN; ++i) {
            m[i] = double(std::max(measured[size_t(kChartFirstNeutral + i)][size_t(ch)], 0.0f));
            t[i] = double(want[size_t(kChartFirstNeutral + i)][size_t(ch)]);
        }
        fitTone(m, t, kN, g0, &r.lift[ch], &r.gamma[ch], &r.gain[ch]);
    }
    // 2. Colour: a matrix with rows adding up to 1, on the tone-corrected patches.
    ChartMatch toneOnly = r;
    std::array<std::array<double, 3>, kChartPatches> a {};
    for (int i = 0; i < kChartPatches; ++i) {
        float c[3] = {measured[size_t(i)][0], measured[size_t(i)][1], measured[size_t(i)][2]};
        applyChartMatch(toneOnly, c);
        for (int ch = 0; ch < 3; ++ch)
            a[size_t(i)][size_t(ch)] = double(c[ch]);
    }
    constexpr double kRidge = 2e-3;
    for (int ch = 0; ch < 3; ++ch) {
        // Unknowns m1, m2 (m3 = 1 - m1 - m2); identity row: 1 at ch.
        double ata[4] = {0, 0, 0, 0}, atb[2] = {0, 0};
        for (int i = 0; i < kChartPatches; ++i) {
            const double d1 = a[size_t(i)][0] - a[size_t(i)][2];
            const double d2 = a[size_t(i)][1] - a[size_t(i)][2];
            const double y = double(want[size_t(i)][size_t(ch)]) - a[size_t(i)][2];
            ata[0] += d1 * d1;
            ata[1] += d1 * d2;
            ata[3] += d2 * d2;
            atb[0] += d1 * y;
            atb[1] += d2 * y;
        }
        ata[2] = ata[1];
        const double id[3] = {ch == 0 ? 1.0 : 0.0, ch == 1 ? 1.0 : 0.0, ch == 2 ? 1.0 : 0.0};
        // Pull of the three entries to the identity row: (m1-I1)^2 + (m2-I2)^2 + (m1+m2-(1-I3))^2.
        ata[0] += kRidge * 2;
        ata[1] += kRidge;
        ata[2] += kRidge;
        ata[3] += kRidge * 2;
        atb[0] += kRidge * (id[0] + (1.0 - id[2]));
        atb[1] += kRidge * (id[1] + (1.0 - id[2]));
        std::vector<double> A(ata, ata + 4), B(atb, atb + 2);
        if (!solveLinear(A, B, 2))
            return r;
        r.mixer[ch * 3 + 0] = float(B[0]);
        r.mixer[ch * 3 + 1] = float(B[1]);
        r.mixer[ch * 3 + 2] = float(1.0 - B[0] - B[1]);
    }
    r.errorAfter = error(&r);
    r.valid = true;
    return r;
}

QVector<std::pair<QString, double>> chartMatchParams(const ChartMatch& match)
{
    QVector<std::pair<QString, double>> v;
    for (int ch = 0; ch < 3; ++ch) {
        v.append({gk::wheelKey(gk::Wheel::Lift, ch), double(match.lift[ch])});
        v.append({gk::wheelKey(gk::Wheel::Gamma, ch), double(match.gamma[ch])});
        v.append({gk::wheelKey(gk::Wheel::Gain, ch), double(match.gain[ch])});
    }
    static const QString* const keys[9] = {&gk::MixerRR, &gk::MixerRG, &gk::MixerRB, &gk::MixerGR,
        &gk::MixerGG, &gk::MixerGB, &gk::MixerBR, &gk::MixerBG, &gk::MixerBB};
    for (int i = 0; i < 9; ++i)
        v.append({*keys[i], double(match.mixer[i])});
    return v;
}

} // namespace kk::colormatch
