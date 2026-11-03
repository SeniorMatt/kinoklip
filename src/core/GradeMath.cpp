// SPDX-License-Identifier: GPL-3.0-or-later
// GradeMath — primaries, curve splines / tables, HSL, the qualifier key and the window matte on
// the CPU. Keep every formula in step with the GLSL in render/GlGrade.cpp (test_graderender
// compares them).
#include "core/GradeMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace kk::grademath {

namespace {

/// Points closer than this in x are treated as one (the spline needs increasing x).
constexpr double kMinPointGap = 1e-6;
/// Distance of the neutral neighbours addCurvePoint() puts around the first point of a hue
/// or lum-vs-sat curve.
constexpr double kNeighbourGap = 1.0 / 12.0;

/// Monotone cubic Hermite interpolation through (xs, ys) at x (xs strictly increasing, at least
/// two points; x outside the range holds the end values).
double monotoneCubic(const std::vector<double>& xs, const std::vector<double>& ys, double x)
{
    const size_t n = xs.size();
    if (x <= xs.front())
        return ys.front();
    if (x >= xs.back())
        return ys.back();
    std::vector<double> d(n - 1);
    for (size_t k = 0; k + 1 < n; ++k)
        d[k] = (ys[k + 1] - ys[k]) / (xs[k + 1] - xs[k]);
    std::vector<double> m(n);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t k = 1; k + 1 < n; ++k)
        m[k] = d[k - 1] * d[k] <= 0.0 ? 0.0 : (d[k - 1] + d[k]) * 0.5;
    // Fritsch-Carlson: limit the tangents so every segment stays monotone (no overshoot).
    for (size_t k = 0; k + 1 < n; ++k) {
        if (d[k] == 0.0) {
            m[k] = 0.0;
            m[k + 1] = 0.0;
            continue;
        }
        const double a = m[k] / d[k];
        const double b = m[k + 1] / d[k];
        const double s = a * a + b * b;
        if (s > 9.0) {
            const double t = 3.0 / std::sqrt(s);
            m[k] = t * a * d[k];
            m[k + 1] = t * b * d[k];
        }
    }
    const size_t k = size_t(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin()) - 1;
    const double h = xs[k + 1] - xs[k];
    const double t = (x - xs[k]) / h;
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * ys[k] + (t3 - 2 * t2 + t) * h * m[k]
        + (-2 * t3 + 3 * t2) * ys[k + 1] + (t3 - t2) * h * m[k + 1];
}

} // namespace

PrimaryValues PrimaryValues::at(const GradeNode& node, int64_t localFrame)
{
    using gradekeys::Wheel;
    PrimaryValues p;
    for (int ch = 0; ch < 4; ++ch) {
        p.lift[ch] = float(node.value(gradekeys::wheelKey(Wheel::Lift, ch), localFrame));
        p.gamma[ch] = float(node.value(gradekeys::wheelKey(Wheel::Gamma, ch), localFrame));
        p.gain[ch] = float(node.value(gradekeys::wheelKey(Wheel::Gain, ch), localFrame));
        p.offset[ch] = float(node.value(gradekeys::wheelKey(Wheel::Offset, ch), localFrame));
    }
    p.contrast = float(node.value(gradekeys::Contrast, localFrame));
    p.pivot = float(node.value(gradekeys::Pivot, localFrame));
    p.saturation = float(node.value(gradekeys::Saturation, localFrame));
    p.hueDegrees = float(node.value(gradekeys::Hue, localFrame));
    p.temperature = float(node.value(gradekeys::Temperature, localFrame));
    p.tint = float(node.value(gradekeys::Tint, localFrame));
    p.lumMix = float(std::clamp(node.value(gradekeys::LumMix, localFrame), 0.0, 1.0));
    if (p.hueDegrees != 0.0f) {
        // Rotation about the grey axis (1, 1, 1) (Rodrigues; keeps (r + g + b) / 3).
        const float a = p.hueDegrees * float(std::numbers::pi) / 180.0f;
        const float cosA = std::cos(a);
        const float sinA = std::sin(a);
        const float k = (1.0f - cosA) / 3.0f;
        const float s = sinA / std::sqrt(3.0f);
        const float m[9] = {cosA + k, k - s, k + s, k + s, cosA + k, k - s, k - s, k + s, cosA + k};
        std::copy(std::begin(m), std::end(m), std::begin(p.hueMatrix));
    }
    return p;
}

bool PrimaryValues::isIdentity() const
{
    for (int ch = 0; ch < 4; ++ch) {
        if (lift[ch] != 0.0f || gamma[ch] != 0.0f || gain[ch] != 1.0f || offset[ch] != 0.0f)
            return false;
    }
    return contrast == 1.0f && saturation == 1.0f && hueDegrees == 0.0f && temperature == 0.0f
        && tint == 0.0f;
}

float liftGammaGain(float x, float lift, float gamma, float gain)
{
    const float v = std::max(gain * (x + lift * (1.0f - x)), 0.0f);
    return std::pow(v, std::exp2(-kGammaRange * gamma));
}

void applyPrimaries(const PrimaryValues& p, float c[3])
{
    // 1. Offset: shifts everything (Resolve's offset wheel, "printer lights" style).
    for (int ch = 0; ch < 3; ++ch)
        c[ch] += p.offset[ch] + p.offset[3];
    // 2. Temperature (red <-> blue) and tint (green <-> magenta), simple channel gains.
    c[0] *= 1.0f + kTemperatureRange * p.temperature;
    c[2] *= 1.0f - kTemperatureRange * p.temperature;
    c[1] *= 1.0f - kTintRange * p.tint;
    // 3. Per-channel lift / gamma / gain.
    for (int ch = 0; ch < 3; ++ch)
        c[ch] = liftGammaGain(c[ch], p.lift[ch], p.gamma[ch], p.gain[ch]);
    // 4. Master wheels: per channel (RGB) or on luma only, preserving the colour (YRGB),
    //    mixed by lum mix (1 = luma only, Resolve's default).
    const float y = luma(c);
    const float y2 = liftGammaGain(y, p.lift[3], p.gamma[3], p.gain[3]);
    for (int ch = 0; ch < 3; ++ch) {
        const float rgbMaster = liftGammaGain(c[ch], p.lift[3], p.gamma[3], p.gain[3]);
        const float lumaMaster = c[ch] + (y2 - y);
        c[ch] = rgbMaster + (lumaMaster - rgbMaster) * p.lumMix;
    }
    // 5. Contrast around the pivot.
    for (int ch = 0; ch < 3; ++ch)
        c[ch] = (c[ch] - p.pivot) * p.contrast + p.pivot;
    // 6. Hue: rotation about the grey axis (PrimaryValues::hueMatrix).
    {
        const float* m = p.hueMatrix;
        const float r = c[0], g = c[1], b = c[2];
        c[0] = m[0] * r + m[1] * g + m[2] * b;
        c[1] = m[3] * r + m[4] * g + m[5] * b;
        c[2] = m[6] * r + m[7] * g + m[8] * b;
    }
    // 7. Saturation around Rec. 709 luma.
    const float y3 = luma(c);
    for (int ch = 0; ch < 3; ++ch)
        c[ch] = y3 + (c[ch] - y3) * p.saturation;
}

double evaluateCurve(const CurvePoints& points, GradeCurve curve, double x)
{
    const double neutralY = curveNeutral(curve);
    if (points.isEmpty())
        return neutralY < 0 ? x : neutralY;
    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(size_t(points.size()) * 3 + 2);
    ys.reserve(xs.capacity());
    auto push = [&](double px, double py) {
        if (!xs.empty() && px - xs.back() < kMinPointGap)
            return;
        xs.push_back(px);
        ys.push_back(py);
    };
    if (isHueCurve(curve)) {
        // Periodic: the points one turn earlier and later around the real ones.
        x -= std::floor(x);
        if (points.size() == 1)
            return points[0].y;
        for (int pass = -1; pass <= 1; ++pass) {
            for (const CurvePoint& p : points)
                push(p.x + pass, p.y);
        }
    } else {
        // Neutral end points where the user's points do not reach the ends.
        if (points.front().x > kMinPointGap)
            push(0.0, neutralY < 0 ? 0.0 : neutralY);
        for (const CurvePoint& p : points)
            push(p.x, p.y);
        if (points.back().x < 1.0 - kMinPointGap)
            push(1.0, neutralY < 0 ? 1.0 : neutralY);
    }
    if (xs.size() == 1)
        return ys[0];
    return monotoneCubic(xs, ys, x);
}

int addCurvePoint(CurvePoints& points, GradeCurve curve, double x, double y)
{
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    if (points.isEmpty() && curveNeutral(curve) >= 0) {
        // Hue / lum-vs-sat: anchor the neighbourhood so only the area around x moves.
        const double n = curveNeutral(curve);
        if (isHueCurve(curve)) {
            const double lo = x - kNeighbourGap - std::floor(x - kNeighbourGap);
            const double hi = x + kNeighbourGap - std::floor(x + kNeighbourGap);
            points.append({lo, n});
            points.append({hi, n});
        } else {
            if (x - kNeighbourGap > 0)
                points.append({x - kNeighbourGap, n});
            if (x + kNeighbourGap < 1)
                points.append({x + kNeighbourGap, n});
        }
    }
    for (int i = 0; i < points.size(); ++i) {
        if (std::abs(points[i].x - x) < kMinPointGap) {
            points[i].y = y;
            return i;
        }
    }
    points.append({x, y});
    std::sort(points.begin(), points.end(),
        [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    for (int i = 0; i < points.size(); ++i) {
        if (points[i].x == x)
            return i;
    }
    return int(points.size()) - 1;
}

CurveTables CurveTables::bake(const GradeCurves& curves)
{
    CurveTables t;
    for (int c = 0; c < kGradeCurveCount; ++c) {
        const GradeCurve kind = GradeCurve(c);
        t.identity[size_t(c)] = curves.isIdentity(kind);
        t.allIdentity = t.allIdentity && t.identity[size_t(c)];
        const bool neutral = t.identity[size_t(c)];
        for (int i = 0; i < kCurveTableSize; ++i) {
            const double x = double(i) / (kCurveTableSize - 1);
            const double y
                = neutral ? (isChannelCurve(kind) ? x : 0.5) : evaluateCurve(curves[kind], kind, x);
            float v = float(y);
            if (kind == GradeCurve::HueHue)
                v = float(y - 0.5);
            else if (!isChannelCurve(kind))
                v = float(2.0 * y);
            t.table[size_t(c)][size_t(i)] = v;
        }
    }
    return t;
}

int CurveTables::mask() const
{
    int m = 0;
    for (int c = 0; c < kGradeCurveCount; ++c) {
        if (!identity[size_t(c)])
            m |= 1 << c;
    }
    return m;
}

float CurveTables::sample(GradeCurve c, float x) const
{
    const float pos = std::clamp(x, 0.0f, 1.0f) * float(kCurveTableSize - 1);
    const int i = std::min(int(pos), kCurveTableSize - 2);
    const float f = pos - float(i);
    const auto& row = table[size_t(c)];
    return row[size_t(i)] + (row[size_t(i) + 1] - row[size_t(i)]) * f;
}

void applyCurves(const CurveTables& t, float c[3])
{
    if (t.allIdentity)
        return;
    auto active = [&t](GradeCurve k) { return !t.identity[size_t(k)]; };
    for (int ch = 0; ch < 3; ++ch) {
        float v = std::clamp(c[ch], 0.0f, 1.0f);
        if (active(GradeCurve::Luma))
            v = t.sample(GradeCurve::Luma, v);
        if (active(GradeCurve(ch + 1)))
            v = t.sample(GradeCurve(ch + 1), v);
        c[ch] = v;
    }
    const bool hueHue = active(GradeCurve::HueHue);
    const bool hueSat = active(GradeCurve::HueSat);
    const bool hueLum = active(GradeCurve::HueLum);
    const bool lumSat = active(GradeCurve::LumSat);
    const bool satSat = active(GradeCurve::SatSat);
    if (!hueHue && !hueSat && !hueLum && !lumSat && !satSat)
        return;
    float hsl[3];
    rgbToHsl(c, hsl);
    const float h = hsl[0];
    const float y = std::clamp(luma(c), 0.0f, 1.0f);
    float factor = 1.0f;
    if (hueSat)
        factor *= t.sample(GradeCurve::HueSat, h);
    if (lumSat)
        factor *= t.sample(GradeCurve::LumSat, y);
    if (satSat)
        factor *= t.sample(GradeCurve::SatSat, hsl[1]);
    if (hueHue) {
        const float shifted = h + t.sample(GradeCurve::HueHue, h);
        hsl[0] = shifted - std::floor(shifted);
    }
    hsl[1] = std::clamp(hsl[1] * factor, 0.0f, 1.0f);
    hslToRgb(hsl, c);
    if (hueLum) {
        // Luminance of the selected hues: the luma moves, the colour differences stay.
        const float y2 = luma(c);
        const float delta = y2 * (t.sample(GradeCurve::HueLum, h) - 1.0f);
        for (int ch = 0; ch < 3; ++ch)
            c[ch] += delta;
    }
}

void rgbToHsl(const float rgb[3], float hsl[3])
{
    const float r = rgb[0], g = rgb[1], b = rgb[2];
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    float h = 0.0f;
    float s = 0.0f;
    if (d > 1e-6f) {
        s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == r)
            h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g)
            h = (b - r) / d + 2.0f;
        else
            h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }
    hsl[0] = h - std::floor(h);
    hsl[1] = s;
    hsl[2] = l;
}

namespace {
float hueToChannel(float p, float q, float t)
{
    t -= std::floor(t);
    if (t < 1.0f / 6.0f)
        return p + (q - p) * 6.0f * t;
    if (t < 0.5f)
        return q;
    if (t < 2.0f / 3.0f)
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}
} // namespace

void hslToRgb(const float hsl[3], float rgb[3])
{
    const float h = hsl[0], s = hsl[1], l = hsl[2];
    if (s <= 0.0f) {
        rgb[0] = rgb[1] = rgb[2] = l;
        return;
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    rgb[0] = hueToChannel(p, q, h + 1.0f / 3.0f);
    rgb[1] = hueToChannel(p, q, h);
    rgb[2] = hueToChannel(p, q, h - 1.0f / 3.0f);
}

float smoothStep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float qualifierKey(const GradeQualifier& q, const float rgb[3])
{
    if (!q.enabled)
        return 1.0f;
    const float c[3] = {std::clamp(rgb[0], 0.0f, 1.0f), std::clamp(rgb[1], 0.0f, 1.0f),
        std::clamp(rgb[2], 0.0f, 1.0f)};
    float hsl[3];
    rgbToHsl(c, hsl);
    float key = 1.0f;
    if (q.useHue) {
        // Circular distance to the centre hue, in turns (0..0.5).
        const float rel = hsl[0] - float(q.hueCenter) + 0.5f;
        const float dist = std::abs(rel - std::floor(rel) - 0.5f);
        const float half = float(q.hueWidth) * 0.5f;
        const float soft = std::max(float(q.hueSoft), kMinSoftness);
        key *= 1.0f - smoothStep(half, half + soft, dist);
    }
    auto range = [](float v, double lo, double hi, double softness) {
        const float soft = std::max(float(softness), kMinSoftness);
        float k = 1.0f;
        if (lo > 0.0)
            k *= smoothStep(float(lo) - soft, float(lo), v);
        if (hi < 1.0)
            k *= 1.0f - smoothStep(float(hi), float(hi) + soft, v);
        return k;
    };
    if (q.useSat)
        key *= range(hsl[1], q.satLow, q.satHigh, q.satSoft);
    if (q.useLum)
        key *= range(hsl[2], q.lumLow, q.lumHigh, q.lumSoft);
    return q.invert ? 1.0f - key : key;
}

} // namespace kk::grademath

namespace kk::grademath {

std::vector<WindowValues> evaluateWindows(
    const GradeNode& node, int64_t localFrame, const PictureGeometry& g)
{
    namespace wk = gradewindowkeys;
    std::vector<WindowValues> out;
    for (const GradeWindow& w : node.windows) {
        if (!w.isRenderable() || int(out.size()) >= kMaxGradeWindows)
            continue;
        WindowValues v;
        v.shape = int(w.shape);
        v.invert = w.invert;
        v.opacity = float(std::clamp(w.value(wk::Opacity, localFrame), 0.0, 1.0));
        v.cx = float(g.originX + w.value(wk::CenterX, localFrame) * g.width);
        v.cy = float(g.originY + w.value(wk::CenterY, localFrame) * g.height);
        const double angle = w.value(wk::Rotation, localFrame) * std::numbers::pi / 180.0;
        v.cosR = float(std::cos(angle));
        v.sinR = float(std::sin(angle));
        const double width = std::abs(w.value(wk::Width, localFrame)) * g.width;
        const double height = std::abs(w.value(wk::Height, localFrame)) * g.height;
        v.halfW = float(width * 0.5);
        v.halfH = float(height * 0.5);
        // Softness in picture heights; at least half a pixel (the antialiasing).
        v.softIn = float(std::max(0.5, std::max(0.0, w.value(wk::SoftIn, localFrame)) * g.height));
        v.softOut
            = float(std::max(0.5, std::max(0.0, w.value(wk::SoftOut, localFrame)) * g.height));
        if (w.shape == GradeWindowShape::Polygon) {
            const int n = std::min(int(w.points.size()), kMaxWindowPoints);
            v.points.reserve(size_t(n) * 2);
            for (int i = 0; i < n; ++i) {
                v.points.push_back(float(w.points[i].x() * width));
                v.points.push_back(float(w.points[i].y() * height));
            }
        }
        out.push_back(std::move(v));
    }
    return out;
}

float windowCoverage(const WindowValues& w, float px, float py)
{
    // Window-local pixel position: undo the centre, then the clockwise rotation.
    const float dx = px - w.cx;
    const float dy = py - w.cy;
    const float lx = w.cosR * dx + w.sinR * dy;
    const float ly = -w.sinR * dx + w.cosR * dy;
    float d = 0.0f; // signed distance in pixels, < 0 inside
    switch (GradeWindowShape(w.shape)) {
    case GradeWindowShape::Circle: {
        const float rx = std::max(w.halfW, 1e-3f);
        const float ry = std::max(w.halfH, 1e-3f);
        const float k = std::sqrt((lx / rx) * (lx / rx) + (ly / ry) * (ly / ry));
        d = (k - 1.0f) * std::min(rx, ry);
        break;
    }
    case GradeWindowShape::Linear: {
        const float qx = std::abs(lx) - w.halfW;
        const float qy = std::abs(ly) - w.halfH;
        const float ox = std::max(qx, 0.0f);
        const float oy = std::max(qy, 0.0f);
        d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f);
        break;
    }
    case GradeWindowShape::Gradient:
        d = ly;
        break;
    case GradeWindowShape::Polygon: {
        const int n = int(w.points.size() / 2);
        float best = 1e30f;
        bool inside = false;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            const float ax = w.points[size_t(j) * 2], ay = w.points[size_t(j) * 2 + 1];
            const float bx = w.points[size_t(i) * 2], by = w.points[size_t(i) * 2 + 1];
            const float ex = bx - ax, ey = by - ay;
            const float wx = lx - ax, wy = ly - ay;
            const float len2 = ex * ex + ey * ey;
            const float t = len2 > 0.0f ? std::clamp((wx * ex + wy * ey) / len2, 0.0f, 1.0f) : 0.0f;
            const float qx = wx - ex * t, qy = wy - ey * t;
            best = std::min(best, qx * qx + qy * qy);
            // Even-odd crossing test of a ray towards +x.
            if ((ay > ly) != (by > ly) && lx < ax + (ly - ay) * ex / ey)
                inside = !inside;
        }
        d = std::sqrt(best) * (inside ? -1.0f : 1.0f);
        break;
    }
    }
    return 1.0f - smoothStep(-w.softIn, w.softOut, d);
}

float windowsMatte(const std::vector<WindowValues>& windows, float px, float py)
{
    if (windows.empty())
        return 1.0f;
    float m = 0.0f;
    for (const WindowValues& w : windows) {
        float c = windowCoverage(w, px, py);
        if (w.invert)
            c = 1.0f - c;
        c *= w.opacity;
        m = m + c - m * c;
    }
    return m;
}

void layerOver(float result[3], const float in[3], const float o[3], float alpha)
{
    for (int ch = 0; ch < 3; ++ch)
        result[ch] = (1.0f - alpha) * result[ch] + alpha * in[ch] + (o[ch] - in[ch]);
}

} // namespace kk::grademath
