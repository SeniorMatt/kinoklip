// SPDX-License-Identifier: GPL-3.0-or-later
// CompMath implementation: node geometry and the pointwise colour formulas.
#include "render/CompMath.h"

#include "compositing/tracking/Homography.h"
#include "core/NodeRegistry.h"

#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <optional>

namespace kk::render::compmath {

namespace k = nodekeys;

namespace {

double num(const ParamSet& p, const QString& key, int64_t frame, double fallback)
{
    return p.value(key, frame, fallback);
}

QTransform translate(QPointF d)
{
    return QTransform::fromTranslate(d.x(), d.y());
}

} // namespace

QTransform transformMatrix(const ParamSet& p, int64_t frame, QSizeF frameSize)
{
    const QPointF mid(frameSize.width() / 2, frameSize.height() / 2);
    const QPointF pivot
        = toFrame(num(p, k::PivotX, frame, 0.5), num(p, k::PivotY, frame, 0.5), frameSize);
    const QPointF centre
        = toFrame(num(p, k::CenterX, frame, 0.5), num(p, k::CenterY, frame, 0.5), frameSize);
    const double size = num(p, k::Size, frame, 1.0);
    const double aspect = num(p, k::Aspect, frame, 1.0);
    const double angle = num(p, k::Angle, frame, 0.0);
    const bool flipH = p.staticValue(k::FlipH, false).toBool();
    const bool flipV = p.staticValue(k::FlipV, false).toBool();
    // QTransform composes left to right (p * A * B applies A first): flip about the frame
    // centre, then scale and turn about the pivot, then move the pivot by Center's offset.
    QTransform m
        = translate(-mid) * QTransform::fromScale(flipH ? -1 : 1, flipV ? -1 : 1) * translate(mid);
    m = m * translate(-pivot) * QTransform::fromScale(size * aspect, size)
        * QTransform().rotate(-angle) // y down: a negative turn is counter-clockwise
        * translate(pivot + (centre - mid));
    return m;
}

QTransform trackerMatrix(const ParamSet& p, int64_t frame, QSizeF frameSize)
{
    const int64_t ref = int64_t(std::llround(num(p, k::RefFrame, 0, 0.0)));
    const QPointF now
        = toFrame(num(p, k::CenterX, frame, 0.5), num(p, k::CenterY, frame, 0.5), frameSize);
    const QPointF then
        = toFrame(num(p, k::CenterX, ref, 0.5), num(p, k::CenterY, ref, 0.5), frameSize);
    QPointF move = now - then;
    if (p.staticValue(k::TrackMode, trackmodes::MatchMove).toString() == trackmodes::Stabilize)
        move = -move;
    return translate(move);
}

std::array<QPointF, 4> planarCorners(const ParamSet& p, int64_t frame, QSizeF frameSize)
{
    // The default region (the middle half of the frame) matches the node descriptor.
    static constexpr double kDefaultX[4] = {0.25, 0.75, 0.75, 0.25};
    static constexpr double kDefaultY[4] = {0.75, 0.75, 0.25, 0.25};
    std::array<QPointF, 4> q;
    for (int i = 0; i < k::kPlanarCorners; ++i) {
        q[size_t(i)] = toFrame(num(p, k::planarCorner(i, false), frame, kDefaultX[i]),
            num(p, k::planarCorner(i, true), frame, kDefaultY[i]), frameSize);
    }
    return q;
}

QTransform planarMatrix(const ParamSet& p, int64_t frame, QSizeF frameSize)
{
    const std::array<QPointF, 4> now = planarCorners(p, frame, frameSize);
    const QString mode = p.staticValue(k::TrackMode, trackmodes::CornerPin).toString();
    if (mode == trackmodes::CornerPin) {
        QPolygonF from, to;
        from << QPointF(0, 0) << QPointF(frameSize.width(), 0)
             << QPointF(frameSize.width(), frameSize.height()) << QPointF(0, frameSize.height());
        for (const QPointF& c : now)
            to << c;
        QTransform t;
        return QTransform::quadToQuad(from, to, t) ? t : QTransform();
    }
    const int64_t ref = int64_t(std::llround(num(p, k::RefFrame, 0, 0.0)));
    const std::array<QPointF, 4> then = planarCorners(p, ref, frameSize);
    tracking::Quad a, b;
    for (size_t i = 0; i < 4; ++i) {
        a[i] = {then[i].x(), then[i].y()};
        b[i] = {now[i].x(), now[i].y()};
    }
    std::optional<tracking::Similarity> sim = tracking::similarityFromQuads(a, b);
    if (!sim)
        return QTransform();
    if (mode == trackmodes::Stabilize) {
        if (sim->scale < 1e-9) // a region collapsed to a point cannot be undone
            return QTransform();
        sim = sim->inverse();
    }
    const double c = std::cos(sim->angleRad) * sim->scale, s = std::sin(sim->angleRad) * sim->scale;
    return QTransform(c, s, -s, c, sim->dx, sim->dy);
}

QTransform mergeForegroundMatrix(const ParamSet& p, int64_t frame, QSizeF frameSize)
{
    const QPointF mid(frameSize.width() / 2, frameSize.height() / 2);
    const QPointF centre
        = toFrame(num(p, k::CenterX, frame, 0.5), num(p, k::CenterY, frame, 0.5), frameSize);
    const double size = num(p, k::Size, frame, 1.0);
    const double angle = num(p, k::Angle, frame, 0.0);
    const double fx = p.staticValue(k::FlipH, false).toBool() ? -1.0 : 1.0;
    const double fy = p.staticValue(k::FlipV, false).toBool() ? -1.0 : 1.0;
    return translate(-mid) * QTransform::fromScale(size * fx, size * fy)
        * QTransform().rotate(-angle) * translate(centre);
}

QRectF resizeRect(const ParamSet& p, int64_t frame, QSizeF frameSize)
{
    double w = std::max(1.0, num(p, k::Width, frame, frameSize.width()));
    double h = std::max(1.0, num(p, k::Height, frame, frameSize.height()));
    if (p.staticValue(k::KeepAspect, false).toBool()) {
        // The whole frame scaled uniformly into the box.
        const double s = std::min(w / frameSize.width(), h / frameSize.height());
        w = frameSize.width() * s;
        h = frameSize.height() * s;
    }
    return QRectF((frameSize.width() - w) / 2, (frameSize.height() - h) / 2, w, h);
}

QRectF letterboxRect(const ParamSet& p, int64_t frame, QSizeF frameSize)
{
    const double target = std::max(0.01, num(p, k::TargetAspect, frame, 2.39));
    const double W = frameSize.width();
    const double H = frameSize.height();
    if (target >= W / H) {
        const double h = W / target; // bars at the top and the bottom
        return QRectF(0, (H - h) / 2, W, h);
    }
    const double w = H * target; // bars at the sides
    return QRectF((W - w) / 2, 0, w, H);
}

QRectF cropRect(const ParamSet& p, int64_t frame, QSizeF frameSize)
{
    const double l = std::max(0.0, num(p, k::Left, frame, 0));
    const double r = std::max(0.0, num(p, k::Right, frame, 0));
    const double t = std::max(0.0, num(p, k::Top, frame, 0));
    const double b = std::max(0.0, num(p, k::Bottom, frame, 0));
    const QRectF rect(QPointF(l, t), QPointF(frameSize.width() - r, frameSize.height() - b));
    return rect.isValid() ? rect : QRectF();
}

MaskData maskOf(const CompNode& node, int64_t frame, QSizeF frameSize)
{
    const ParamSet& p = node.params;
    MaskData m;
    m.id = node.id;
    m.shape = node.type == nodetypes::EllipseMask ? MaskShape::Ellipse
        : node.type == nodetypes::PolygonMask     ? MaskShape::Polygon
                                                  : MaskShape::Rectangle;
    m.invert = p.staticValue(k::Invert, false).toBool();
    m.closed = true;
    const double W = frameSize.width();
    const double H = frameSize.height();
    auto set = [&m](const QString& key, double v) { m.params.values[key].base = v; };
    set(maskkeys::PosX, (num(p, k::CenterX, frame, 0.5) - 0.5) * W);
    set(maskkeys::PosY, (0.5 - num(p, k::CenterY, frame, 0.5)) * H);
    set(maskkeys::Rotation, -num(p, k::Angle, frame, 0.0));
    set(maskkeys::Opacity, std::clamp(num(p, k::Level, frame, 1.0), 0.0, 1.0) * 100.0);
    const double soft = std::max(0.0, num(p, k::SoftEdge, frame, 0.0)) * W * 0.5;
    set(maskkeys::FeatherIn, soft);
    set(maskkeys::FeatherOut, soft);
    if (m.shape == MaskShape::Polygon) {
        const double size = num(p, k::Size, frame, 1.0);
        set(maskkeys::ScaleX, size);
        set(maskkeys::ScaleY, size);
        m.path = node.path;
    } else {
        set(maskkeys::Width, std::max(0.0, num(p, k::Width, frame, 0.5)) * W);
        set(maskkeys::Height, std::max(0.0, num(p, k::Height, frame, 0.5)) * H);
    }
    return m;
}

bool CorrectorParams::isIdentity() const
{
    return lift == 0 && gamma == 1 && gain == 1 && saturation == 1 && inBlack == 0 && inWhite == 1
        && levelGamma == 1 && outBlack == 0 && outWhite == 1;
}

CorrectorParams correctorParams(const ParamSet& p, int64_t frame)
{
    CorrectorParams c;
    c.lift = float(num(p, k::Lift, frame, 0));
    c.gamma = float(std::max(0.01, num(p, k::Gamma, frame, 1)));
    c.gain = float(num(p, k::Gain, frame, 1));
    c.saturation = float(std::max(0.0, num(p, k::Saturation, frame, 1)));
    c.inBlack = float(num(p, k::InBlack, frame, 0));
    c.inWhite = float(num(p, k::InWhite, frame, 1));
    c.levelGamma = float(std::max(0.01, num(p, k::LevelGamma, frame, 1)));
    c.outBlack = float(num(p, k::OutBlack, frame, 0));
    c.outWhite = float(num(p, k::OutWhite, frame, 1));
    return c;
}

void applyCorrector(const CorrectorParams& p, float rgb[3])
{
    const float range = std::max(1e-4f, p.inWhite - p.inBlack);
    for (int i = 0; i < 3; ++i) {
        float c = std::clamp((rgb[i] - p.inBlack) / range, 0.0f, 1.0f);
        c = std::pow(c, 1.0f / p.levelGamma);
        c = p.outBlack + c * (p.outWhite - p.outBlack);
        c = p.gain * (c + p.lift * (1.0f - c));
        c = std::pow(std::max(c, 0.0f), 1.0f / p.gamma);
        rgb[i] = c;
    }
    const float l = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    for (int i = 0; i < 3; ++i)
        rgb[i] = std::clamp(l + (rgb[i] - l) * p.saturation, 0.0f, 1.0f);
}

std::array<int, 4> channelSources(const ParamSet& p)
{
    const QString* keys[4] = {&k::ToRed, &k::ToGreen, &k::ToBlue, &k::ToAlpha};
    std::array<int, 4> out {0, 1, 2, 3};
    for (int i = 0; i < 4; ++i) {
        const int s = channelsources::index(p.staticValue(*keys[i]).toString());
        if (s >= 0)
            out[size_t(i)] = s;
    }
    return out;
}

void applyChannels(const std::array<int, 4>& src, float rgba[4])
{
    const float in[7] = {rgba[0], rgba[1], rgba[2], rgba[3],
        0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2], 0.0f, 1.0f};
    for (int i = 0; i < 4; ++i)
        rgba[i] = in[std::clamp(src[size_t(i)], 0, 6)];
}

namespace {

float colorDodge(float cb, float cs)
{
    if (cb <= 0.0f)
        return 0.0f;
    return cs >= 1.0f ? 1.0f : std::min(1.0f, cb / (1.0f - cs));
}

float colorBurn(float cb, float cs)
{
    if (cb >= 1.0f)
        return 1.0f;
    return cs <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - cb) / cs);
}

} // namespace

float blendChannel(int mode, float cb, float cs)
{
    switch (mode) {
    case 1: // screen
        return cb + cs - cb * cs;
    case 2: // multiply
        return cb * cs;
    case 3: // add
        return std::min(1.0f, cb + cs);
    case 4: // overlay = hard light with the layers swapped
        return cb <= 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
    case 5: // darken
        return std::min(cb, cs);
    case 6: // lighten
        return std::max(cb, cs);
    case 7: // difference
        return std::abs(cb - cs);
    case 8: // subtract
        return std::max(0.0f, cb - cs);
    case 9: // hard light
        return cs <= 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
    case 10: { // soft light (the W3C compositing formula)
        if (cs <= 0.5f)
            return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
        const float d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : std::sqrt(cb);
        return cb + (2.0f * cs - 1.0f) * (d - cb);
    }
    case 11: // color dodge
        return colorDodge(cb, cs);
    case 12: // color burn
        return colorBurn(cb, cs);
    case 13: // exclusion
        return cb + cs - 2.0f * cb * cs;
    case 14: // linear burn
        return std::max(0.0f, cb + cs - 1.0f);
    case 15: // linear light
        return std::clamp(cb + 2.0f * cs - 1.0f, 0.0f, 1.0f);
    case 16: // vivid light
        return cs <= 0.5f ? colorBurn(cb, 2.0f * cs) : colorDodge(cb, 2.0f * cs - 1.0f);
    case 17: // pin light
        return cs <= 0.5f ? std::min(cb, 2.0f * cs) : std::max(cb, 2.0f * cs - 1.0f);
    case 18: // hard mix
        return cb + cs >= 1.0f ? 1.0f : 0.0f;
    default:
        return cs;
    }
}

void blendPremultiplied(int mode, const float b[4], const float s[4], float o[4])
{
    if (mode == 0) {
        for (int i = 0; i < 4; ++i)
            o[i] = s[i] + b[i] * (1.0f - s[3]);
        return;
    }
    for (int i = 0; i < 3; ++i) {
        const float cb = b[3] > 0 ? b[i] / b[3] : 0.0f;
        const float cs = s[3] > 0 ? s[i] / s[3] : 0.0f;
        o[i] = s[i] * (1 - b[3]) + b[i] * (1 - s[3]) + s[3] * b[3] * blendChannel(mode, cb, cs);
    }
    o[3] = s[3] + b[3] * (1 - s[3]);
}

const char* const kBlendFunctionGlsl = R"(
float kkDodge(float b, float s) { return b <= 0.0 ? 0.0 : (s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s))); }
float kkBurn(float b, float s) { return b >= 1.0 ? 1.0 : (s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s)); }
float kkBlend(int m, float b, float s)
{
    if (m == 1) return b + s - b * s;
    if (m == 2) return b * s;
    if (m == 3) return min(1.0, b + s);
    if (m == 4) return b <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    if (m == 5) return min(b, s);
    if (m == 6) return max(b, s);
    if (m == 7) return abs(b - s);
    if (m == 8) return max(0.0, b - s);
    if (m == 9) return s <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    if (m == 10) {
        if (s <= 0.5) return b - (1.0 - 2.0 * s) * b * (1.0 - b);
        float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
        return b + (2.0 * s - 1.0) * (d - b);
    }
    if (m == 11) return kkDodge(b, s);
    if (m == 12) return kkBurn(b, s);
    if (m == 13) return b + s - 2.0 * b * s;
    if (m == 14) return max(0.0, b + s - 1.0);
    if (m == 15) return clamp(b + 2.0 * s - 1.0, 0.0, 1.0);
    if (m == 16) return s <= 0.5 ? kkBurn(b, 2.0 * s) : kkDodge(b, 2.0 * s - 1.0);
    if (m == 17) return s <= 0.5 ? min(b, 2.0 * s) : max(b, 2.0 * s - 1.0);
    if (m == 18) return b + s >= 1.0 ? 1.0 : 0.0;
    return s;
}
vec3 kkBlend3(int m, vec3 b, vec3 s)
{
    return vec3(kkBlend(m, b.r, s.r), kkBlend(m, b.g, s.g), kkBlend(m, b.b, s.b));
}
)";

std::array<float, 4> colorOf(const ParamSet& p, const QString& key, const QColor& fallback)
{
    const QVariant v = p.staticValue(key);
    const QColor c
        = v.canConvert<QColor>() && v.value<QColor>().isValid() ? v.value<QColor>() : fallback;
    const float a = float(c.alphaF());
    return {float(c.redF()) * a, float(c.greenF()) * a, float(c.blueF()) * a, a};
}

GeneratorFrame backgroundFrame(const ParamSet& p, int64_t frame, QSizeF frameSize)
{
    const QString mode = p.staticValue(k::Mode, backgroundmodes::Solid).toString();
    const auto c1 = colorOf(p, k::Color, Qt::black);
    const auto c2 = colorOf(p, k::Color2, Qt::white);
    GeneratorFrame g;
    if (mode == backgroundmodes::Gradient) {
        g.kind = GeneratorFrame::Kind::Linear;
        g.colors[0] = c1;
        g.colors[1] = c2;
        const double a = num(p, k::GradientAngle, frame, 0.0);
        g.angleDeg = -a; // GeneratorFrame turns clockwise, the node counter-clockwise
        const double r = a * M_PI / 180.0;
        g.length = std::max(1.0,
            std::abs(frameSize.width() * std::cos(r)) + std::abs(frameSize.height() * std::sin(r)));
    } else if (mode == backgroundmodes::FourCorner) {
        g.kind = GeneratorFrame::Kind::FourColor;
        g.colors = {c1, c2, colorOf(p, k::Color3, Qt::red), colorOf(p, k::Color4, Qt::blue)};
    } else {
        g.kind = GeneratorFrame::Kind::FourColor;
        g.colors = {c1, c1, c1, c1};
    }
    return g;
}

TitleStyle textStyle(
    const ParamSet& p, int64_t frame, QSize canvas, double scale, QSizeF frameSize, QPointF* offset)
{
    TitleStyle s;
    s.text = p.staticValue(k::Text).toString();
    s.font = p.staticValue(k::Font, QStringLiteral("Sans Serif")).toString();
    s.bold = p.staticValue(k::Bold, false).toBool();
    s.italic = p.staticValue(k::Italic, false).toBool();
    const QVariant color = p.staticValue(k::Color, QColor(Qt::white));
    s.color = color.value<QColor>().isValid() ? color.value<QColor>() : QColor(Qt::white);
    const QVariant outline = p.staticValue(k::OutlineColor, QColor(Qt::black));
    s.outlineColor
        = outline.value<QColor>().isValid() ? outline.value<QColor>() : QColor(Qt::black);
    s.shadow = p.staticValue(k::Shadow, false).toBool();
    s.size = std::max(1.0, num(p, k::FontSize, frame, 96.0));
    s.outlineWidth = std::max(0.0, num(p, k::OutlineWidth, frame, 0.0));
    s.writeOnStart = std::clamp(num(p, k::WriteOnStart, frame, 0.0), 0.0, 1.0);
    s.writeOnEnd = std::clamp(num(p, k::WriteOnEnd, frame, 1.0), 0.0, 1.0);
    s.canvas = canvas;
    s.scale = scale;
    const QPointF c
        = toFrame(num(p, k::CenterX, frame, 0.5), num(p, k::CenterY, frame, 0.5), frameSize);
    *offset = QPointF(
        (c.x() - frameSize.width() / 2) * scale, (c.y() - frameSize.height() / 2) * scale);
    return s;
}

NoiseParams noiseParams(const ParamSet& p, int64_t frame, QSizeF frameSize, double fps)
{
    NoiseParams n;
    const QPointF c
        = toFrame(num(p, k::CenterX, frame, 0.5), num(p, k::CenterY, frame, 0.5), frameSize);
    n.centreX = c.x();
    n.centreY = c.y();
    const double a = num(p, k::Angle, frame, 0.0) * M_PI / 180.0;
    n.cosA = std::cos(a);
    n.sinA = std::sin(a);
    n.scale = std::max(0.01, num(p, k::Scale, frame, 10.0));
    n.detail = std::clamp(int(std::lround(num(p, k::Detail, frame, 2))), 1, 8);
    n.contrast = num(p, k::Contrast, frame, 1.0);
    n.brightness = num(p, k::Brightness, frame, 0.0);
    n.time = num(p, k::Seethe, frame, 0.0) * double(frame) / std::max(1.0, fps);
    n.seed = uint32_t(std::max(0.0, num(p, k::Seed, frame, 0.0)));
    n.color1 = colorOf(p, k::Color, Qt::black);
    n.color2 = colorOf(p, k::Color2, Qt::white);
    return n;
}

namespace {

// The GLSL hash / lattice / value noise of GlComposition's noise pass, in C++.
uint32_t noiseHash3(uint32_t x, uint32_t y, uint32_t z, uint32_t seed)
{
    uint32_t h = (x * 0x8da6b343u) ^ (y * 0xd8163841u) ^ (z * 0xcb1ab31fu) ^ (seed * 0x165667b1u);
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

float lattice(int x, int y, int z, uint32_t seed)
{
    return float(noiseHash3(uint32_t(x), uint32_t(y), uint32_t(z), seed) & 0xffffu) / 65535.0f;
}

float valueNoise(double px, double py, double pz, uint32_t seed)
{
    const double fx = std::floor(px), fy = std::floor(py), fz = std::floor(pz);
    const int ix = int(fx), iy = int(fy), iz = int(fz);
    const auto s = [](double t) { return float(t * t * (3.0 - 2.0 * t)); };
    const float ux = s(px - fx), uy = s(py - fy), uz = s(pz - fz);
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float a = lerp(lerp(lattice(ix, iy, iz, seed), lattice(ix + 1, iy, iz, seed), ux),
        lerp(lattice(ix, iy + 1, iz, seed), lattice(ix + 1, iy + 1, iz, seed), ux), uy);
    const float b = lerp(lerp(lattice(ix, iy, iz + 1, seed), lattice(ix + 1, iy, iz + 1, seed), ux),
        lerp(lattice(ix, iy + 1, iz + 1, seed), lattice(ix + 1, iy + 1, iz + 1, seed), ux), uy);
    return lerp(a, b, uz);
}

} // namespace

float noiseAt(const NoiseParams& n, double x, double y, QSizeF frameSize)
{
    const double dx = x - n.centreX;
    const double dy = y - n.centreY;
    const double k = n.scale / frameSize.width();
    double qx = (n.cosA * dx + n.sinA * dy) * k;
    double qy = (-n.sinA * dx + n.cosA * dy) * k;
    double sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < n.detail; ++o) {
        sum += amp * valueNoise(qx + o * 17.0, qy + o * 17.0, n.time + o * 17.0, n.seed);
        norm += amp;
        qx *= 2.0;
        qy *= 2.0;
        amp *= 0.5;
    }
    return float(std::clamp((sum / norm - 0.5) * n.contrast + 0.5 + n.brightness, 0.0, 1.0));
}

} // namespace kk::render::compmath
