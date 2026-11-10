// SPDX-License-Identifier: GPL-3.0-or-later
// Generators — parameter resolution and the CPU raster of the built-in generators.
//
// The GPU path (GlGenerators.cpp) evaluates the same formulas in GLSL; keep the two in step
// (tests/render/test_generators.cpp compares them).
#include "render/Generators.h"

#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/ParamKeys.h"
#include "render/TestPatterns.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render {

namespace g = generators;

namespace {

std::array<float, 4> premultiplied(const QColor& c)
{
    const float a = float(c.alphaF());
    return {float(c.redF()) * a, float(c.greenF()) * a, float(c.blueF()) * a, a};
}

/// Descriptor default of static parameter @p key of generator @p id.
QVariant descriptorDefault(const QString& id, const QString& key)
{
    if (const EffectDescriptor* d = EffectRegistry::instance().find(id)) {
        if (const ParamDescriptor* p = d->param(key))
            return p->isScalar() ? QVariant(p->def) : p->defStatic;
    }
    return {};
}

QVariant staticParam(const ClipData& clip, const QString& key)
{
    const auto it = clip.title.statics.constFind(key);
    return it != clip.title.statics.constEnd() ? *it : descriptorDefault(clip.generator, key);
}

QColor colorParam(const ClipData& clip, const QString& key)
{
    const QColor c = staticParam(clip, key).value<QColor>();
    return c.isValid() ? c : QColor(Qt::black);
}

double numberParam(const ClipData& clip, const QString& key, int64_t local)
{
    const auto it = clip.title.values.constFind(key);
    if (it != clip.title.values.constEnd())
        return it->valueAt(local);
    return descriptorDefault(clip.generator, key).toDouble();
}

int fpsBase(Rational fps)
{
    return std::max(1, int(std::lround(fps.toDouble())));
}

std::array<float, 4> mix4(const std::array<float, 4>& a, const std::array<float, 4>& b, double t)
{
    std::array<float, 4> r;
    for (int i = 0; i < 4; ++i)
        r[size_t(i)] = float(a[size_t(i)] + (b[size_t(i)] - a[size_t(i)]) * t);
    return r;
}

/// Premultiplied colour of a pattern generator at timeline pixel (x, y) of @p frame.
std::array<float, 4> patternAt(const GeneratorFrame& gf, double x, double y, QSizeF frame)
{
    const double u = x / frame.width();
    const double v = y / frame.height();
    switch (gf.kind) {
    case GeneratorFrame::Kind::FourColor:
        return mix4(mix4(gf.colors[0], gf.colors[1], u), mix4(gf.colors[2], gf.colors[3], u), v);
    case GeneratorFrame::Kind::Linear: {
        const double a = gf.angleDeg * std::numbers::pi / 180.0;
        const double px = x - frame.width() / 2 - gf.centerX;
        const double py = y - frame.height() / 2 - gf.centerY;
        const double t
            = std::clamp((px * std::cos(a) + py * std::sin(a)) / gf.length + 0.5, 0.0, 1.0);
        return mix4(gf.colors[0], gf.colors[1], t);
    }
    case GeneratorFrame::Kind::Radial: {
        const double px = x - frame.width() / 2 - gf.centerX;
        const double py = y - frame.height() / 2 - gf.centerY;
        return mix4(gf.colors[0], gf.colors[1], std::clamp(std::hypot(px, py) / gf.radius, 0., 1.));
    }
    case GeneratorFrame::Kind::Bars: {
        const QRgb c = gf.ebuBars ? testpattern::ebuBarsAt(u) : colorBarsAt(u, v, gf.fullBars);
        return {qRed(c) / 255.f, qGreen(c) / 255.f, qBlue(c) / 255.f, 1.f};
    }
    case GeneratorFrame::Kind::GreyScale: {
        const double t = gf.vertical ? v : u;
        const double level = gf.steps < 2
            ? t
            : std::min(std::floor(t * gf.steps), double(gf.steps - 1)) / (gf.steps - 1);
        return {float(level), float(level), float(level), 1.f};
    }
    case GeneratorFrame::Kind::Noise: {
        const auto cx = uint32_t(std::max(0.0, std::floor(x / gf.grain)));
        const auto cy = uint32_t(std::max(0.0, std::floor(y / gf.grain)));
        auto channel = [&](uint32_t salt) {
            const double n = double(noiseHash(cx, cy, gf.seed + salt) & 0xffffu) / 65535.0;
            return float(std::clamp(gf.brightness + gf.amount * (n - 0.5), 0.0, 1.0));
        };
        const float r = channel(0);
        return gf.monochrome ? std::array<float, 4> {r, r, r, 1.f}
                             : std::array<float, 4> {r, channel(1), channel(2), 1.f};
    }
    case GeneratorFrame::Kind::Countdown: {
        // Background, sweep wedge inside the circle (from 12 o'clock, clockwise), two rings
        // and the cross hair, all in timeline pixels relative to the frame centre.
        const double px = x - frame.width() / 2;
        const double py = y - frame.height() / 2;
        const double h = frame.height();
        const double r = std::hypot(px, py);
        const double ring = kCountdownRing * h;
        const double outer = kCountdownRadius * h;
        if (std::abs(r - outer) < ring || std::abs(r - outer * 0.86) < ring * 0.5
            || std::abs(px) < ring * 0.4 || std::abs(py) < ring * 0.4)
            return gf.colors[2];
        if (r < outer) {
            double angle = std::atan2(px, -py) / (2 * std::numbers::pi); // 0 at 12, clockwise
            if (angle < 0)
                angle += 1.0;
            if (angle < gf.sweep)
                return gf.colors[1];
        }
        return gf.colors[0];
    }
    case GeneratorFrame::Kind::Bt2111: {
        const auto c = testpattern::bt2111At(testpattern::Bt2111(gf.bt2111), u, v);
        return {c.r, c.g, c.b, 1.f};
    }
    case GeneratorFrame::Kind::YCbCr: {
        // Horizontal ramp (the grey scale's orientation flag turns it vertical); three bands
        // stacked across the other axis when no single channel is chosen.
        const double along = gf.vertical ? v : u;
        const double across = gf.vertical ? u : v;
        const int band = gf.ycbcrBand >= 0 ? gf.ycbcrBand : std::min(2, int(across * 3.0));
        const auto c = testpattern::ycbcrRampAt(band, float(along));
        return {c.r, c.g, c.b, 1.f};
    }
    case GeneratorFrame::Kind::NoiseGradient: {
        const float n = testpattern::noiseGradientValue(float(x), float(y), float(gf.noiseScale),
            gf.noiseOctaves, float(gf.noiseEvolution), gf.seed);
        const float t = std::clamp(float((n - 0.5f) * gf.noiseContrast + 0.5f), 0.f, 1.f);
        return mix4(gf.colors[0], gf.colors[1], t);
    }
    case GeneratorFrame::Kind::Window: {
        const double px = x - frame.width() / 2 - gf.centerX;
        const double py = y - frame.height() / 2 - gf.centerY;
        const double hw = gf.windowW / 2, hh = gf.windowH / 2;
        // Signed distance outside the shape in px (negative inside).
        const double d = gf.windowEllipse ? (std::hypot(px / hw, py / hh) - 1.0) * std::min(hw, hh)
                                          : std::max(std::abs(px) - hw, std::abs(py) - hh);
        const double cover
            = gf.softness > 0 ? std::clamp(0.5 - d / gf.softness, 0.0, 1.0) : (d < 0 ? 1.0 : 0.0);
        return mix4(gf.colors[0], gf.colors[1], cover);
    }
    case GeneratorFrame::Kind::Grid: {
        const float cover = testpattern::gridCoverage(float(x), float(y),
            float(frame.width() / gf.gridColumns), float(frame.height() / gf.gridRows),
            float(gf.lineWidth), gf.majorEvery, float(gf.majorWidth));
        return mix4(gf.colors[0], gf.colors[1], cover);
    }
    case GeneratorFrame::Kind::Unknown:
    case GeneratorFrame::Kind::Timecode:
        break;
    }
    return {0, 0, 0, 0};
}

} // namespace

QRgb colorBarsAt(double u, double v, bool fullField)
{
    // 75 % bars and the full-field 100 % set; studio black is 0 in our full-range RGB.
    static constexpr QRgb kTop[7]
        = {0xbfbfbf, 0xbfbf00, 0x00bfbf, 0x00bf00, 0xbf00bf, 0xbf0000, 0x0000bf};
    static constexpr QRgb kMiddle[7]
        = {0x0000bf, 0x000000, 0xbf00bf, 0x000000, 0x00bfbf, 0x000000, 0xbfbfbf};
    static constexpr QRgb kFull[8]
        = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000};
    u = std::clamp(u, 0.0, 0.999999);
    v = std::clamp(v, 0.0, 0.999999);
    if (fullField)
        return 0xff000000u | kFull[int(u * 8)];
    if (v < 2.0 / 3.0)
        return 0xff000000u | kTop[int(u * 7)];
    if (v < 0.75)
        return 0xff000000u | kMiddle[int(u * 7)];
    // Bottom row in bar units: -I, white, +Q, black (5/4 bar each), PLUGE (black, +4 %,
    // +8 % in thirds of a bar), black.
    const double x = u * 7.0;
    if (x < 1.25)
        return 0xff00214cu;
    if (x < 2.5)
        return 0xffffffffu;
    if (x < 3.75)
        return 0xff32006au;
    if (x < 5.0)
        return 0xff000000u;
    if (x < 5.0 + 1.0 / 3.0)
        return 0xff000000u;
    if (x < 5.0 + 2.0 / 3.0)
        return 0xff0a0a0au;
    if (x < 6.0)
        return 0xff141414u;
    return 0xff000000u;
}

QString generatorTimecode(int64_t frames, Rational fps)
{
    const int base = fpsBase(fps);
    const bool negative = frames < 0;
    int64_t f = negative ? -frames : frames;
    const int64_t ff = f % base;
    f /= base;
    return QStringLiteral("%1%2:%3:%4:%5")
        .arg(negative ? QStringLiteral("-") : QString())
        .arg(f / 3600, 2, 10, QLatin1Char('0'))
        .arg(f / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(f % 60, 2, 10, QLatin1Char('0'))
        .arg(ff, 2, 10, QLatin1Char('0'));
}

GeneratorFrame resolveGenerator(
    const ClipData& clip, int64_t local, int64_t timelineFrame, Rational fps)
{
    using K = GeneratorFrame::Kind;
    GeneratorFrame gf;
    const QString& id = clip.generator;
    if (id == g::FourColorGradient) {
        gf.kind = K::FourColor;
        gf.colors = {premultiplied(colorParam(clip, g::Color1)),
            premultiplied(colorParam(clip, g::Color2)), premultiplied(colorParam(clip, g::Color3)),
            premultiplied(colorParam(clip, g::Color4))};
    } else if (id == g::LinearGradient || id == g::RadialGradient) {
        gf.kind = id == g::LinearGradient ? K::Linear : K::Radial;
        gf.colors[0] = premultiplied(colorParam(clip, g::Color1));
        gf.colors[1] = premultiplied(colorParam(clip, g::Color2));
        gf.centerX = numberParam(clip, g::CenterX, local);
        gf.centerY = numberParam(clip, g::CenterY, local);
        gf.angleDeg = numberParam(clip, g::Angle, local);
        gf.length = std::max(1.0, numberParam(clip, g::Length, local));
        gf.radius = std::max(1.0, numberParam(clip, g::Radius, local));
    } else if (id == g::ColorBars) {
        gf.kind = K::Bars;
        gf.fullBars = staticParam(clip, g::Style).toString() == QLatin1String("full");
    } else if (id == g::EbuBars) {
        gf.kind = K::Bars;
        gf.ebuBars = true;
    } else if (id == g::Steps100mV) {
        // Eight steps of 100 mV of the 700 mV signal: 0, 100, ... 700 mV = n / 7 of full scale.
        gf.kind = K::GreyScale;
        gf.steps = 8;
        gf.vertical = staticParam(clip, g::Orientation).toString() == QLatin1String("vertical");
    } else if (id == g::Bt2111HlgNarrow || id == g::Bt2111PqNarrow || id == g::Bt2111PqFull) {
        gf.kind = K::Bt2111;
        gf.bt2111 = int(id == g::Bt2111HlgNarrow ? testpattern::Bt2111::HlgNarrow
                : id == g::Bt2111PqNarrow        ? testpattern::Bt2111::PqNarrow
                                                 : testpattern::Bt2111::PqFull);
    } else if (id == g::Window) {
        gf.kind = K::Window;
        gf.windowEllipse = staticParam(clip, g::Shape).toString() == QLatin1String("ellipse");
        gf.windowW = std::max(1.0, numberParam(clip, g::Width, local));
        gf.windowH = std::max(1.0, numberParam(clip, g::Height, local));
        gf.centerX = numberParam(clip, g::CenterX, local);
        gf.centerY = numberParam(clip, g::CenterY, local);
        gf.softness = std::max(0.0, numberParam(clip, g::Softness, local));
        gf.colors[1] = premultiplied(colorParam(clip, g::Color1));
        gf.colors[0] = premultiplied(colorParam(clip, g::Color2));
    } else if (id == g::YcbcrRamp) {
        gf.kind = K::YCbCr;
        gf.vertical = staticParam(clip, g::Orientation).toString() == QLatin1String("vertical");
        const QString channel = staticParam(clip, g::Channel).toString();
        gf.ycbcrBand = channel == QLatin1String("y") ? 0
            : channel == QLatin1String("cb")         ? 1
            : channel == QLatin1String("cr")         ? 2
                                                     : -1;
    } else if (id == g::NoiseGradient) {
        gf.kind = K::NoiseGradient;
        gf.colors[0] = premultiplied(colorParam(clip, g::Color1));
        gf.colors[1] = premultiplied(colorParam(clip, g::Color2));
        gf.noiseScale = std::max(1.0, numberParam(clip, g::Scale, local));
        gf.noiseOctaves = std::clamp(int(std::lround(numberParam(clip, g::Detail, local))), 1, 8);
        gf.noiseContrast = std::max(0.0, numberParam(clip, g::Contrast, local) / 100.0);
        gf.noiseEvolution = numberParam(clip, g::Evolution, local);
        gf.seed = uint32_t(std::lround(numberParam(clip, g::Seed, local)));
    } else if (id == g::Grid) {
        gf.kind = K::Grid;
        gf.gridColumns = std::max(1, int(std::lround(numberParam(clip, g::Columns, local))));
        gf.gridRows = std::max(1, int(std::lround(numberParam(clip, g::Rows, local))));
        gf.lineWidth = std::max(0.0, numberParam(clip, g::LineWidth, local));
        gf.majorWidth = std::max(0.0, numberParam(clip, g::MajorWidth, local));
        gf.majorEvery = std::max(0, int(std::lround(numberParam(clip, g::MajorEvery, local))));
        gf.colors[1] = premultiplied(colorParam(clip, g::LineColor));
        QColor bg = colorParam(clip, g::Background);
        bg.setAlphaF(std::clamp(numberParam(clip, g::BackgroundOpacity, local) / 100.0, 0.0, 1.0));
        gf.colors[0] = premultiplied(bg);
    } else if (id == g::GreyScale) {
        gf.kind = K::GreyScale;
        gf.steps = int(std::lround(numberParam(clip, g::Steps, local)));
        gf.vertical = staticParam(clip, g::Orientation).toString() == QLatin1String("vertical");
    } else if (id == g::Noise) {
        gf.kind = K::Noise;
        gf.monochrome = staticParam(clip, g::Monochrome).toBool();
        gf.brightness = std::clamp(numberParam(clip, g::Brightness, local) / 100.0, 0.0, 1.0);
        gf.amount = std::clamp(numberParam(clip, g::Amount, local) / 100.0, 0.0, 1.0);
        gf.grain = std::max(1.0, numberParam(clip, g::Grain, local));
        const auto seed = uint32_t(std::lround(numberParam(clip, g::Seed, local)));
        // Animated noise changes with the clip-local frame (so a copy of the clip shows the
        // same grain), still noise keeps the seed.
        gf.seed = staticParam(clip, g::Animated).toBool()
            ? noiseHash(uint32_t(local), seed, 0x51ed270bu)
            : seed;
    } else if (id == g::Countdown) {
        gf.kind = K::Countdown;
        const int base = fpsBase(fps);
        const int64_t startParam = std::lround(numberParam(clip, g::StartSeconds, local));
        const int64_t start
            = startParam > 0 ? startParam : std::max<int64_t>(1, (clip.duration + base - 1) / base);
        const int64_t elapsed = std::max<int64_t>(0, local);
        const int64_t number = std::max<int64_t>(1, start - elapsed / base);
        gf.sweep = double(elapsed % base) / base;
        const QColor fg = colorParam(clip, keys::Color);
        gf.colors[0] = premultiplied(colorParam(clip, g::Background));
        gf.colors[1] = premultiplied(colorParam(clip, g::Accent));
        gf.colors[2] = premultiplied(fg);
        gf.textStyle.text = QString::number(number);
        gf.textStyle.font = staticParam(clip, keys::Font).toString();
        gf.textStyle.bold = true;
        gf.textStyle.color = fg;
        gf.textStyle.size = 0; // set from the frame height by the renderer (kCountdownNumberSize)
    } else if (id == g::Timecode) {
        gf.kind = K::Timecode;
        const bool clipTc = staticParam(clip, g::Source).toString() == QLatin1String("clip");
        const int64_t value = (clipTc ? local : timelineFrame)
            + std::lround(numberParam(clip, g::StartFrame, local));
        gf.textStyle.text = staticParam(clip, g::Prefix).toString() + generatorTimecode(value, fps);
        gf.textStyle.font = staticParam(clip, keys::Font).toString();
        gf.textStyle.color = colorParam(clip, keys::Color);
        gf.textStyle.bgEnabled = staticParam(clip, keys::BgEnabled).toBool();
        gf.textStyle.bgColor = colorParam(clip, keys::BgColor);
        gf.textStyle.bgOpacity = std::clamp(numberParam(clip, keys::BgOpacity, local), 0.0, 100.0);
        gf.textStyle.size = std::max(1.0, numberParam(clip, keys::Size, local));
        gf.textOffsetY = numberParam(clip, g::OffsetY, local) / 100.0;
    }
    gf.textStyle.outlineColor = QColor(Qt::black);
    return gf;
}

double countdownNumberSize(double frameHeight)
{
    return frameHeight * kCountdownNumberSize;
}

QImage renderGeneratorImage(GeneratorFrame gf, QSize canvas, QSizeF frameSize)
{
    QImage image(canvas.expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (gf.kind == GeneratorFrame::Kind::Unknown)
        return image;
    const double sx = frameSize.width() / image.width();
    const double sy = frameSize.height() / image.height();
    if (gf.hasPattern()) {
        for (int y = 0; y < image.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const auto c = patternAt(gf, (x + 0.5) * sx, (y + 0.5) * sy, frameSize);
                row[x] = qRgba(int(std::lround(c[0] * 255)), int(std::lround(c[1] * 255)),
                    int(std::lround(c[2] * 255)), int(std::lround(c[3] * 255)));
            }
        }
    }
    if (!gf.textStyle.text.isEmpty()) {
        if (gf.kind == GeneratorFrame::Kind::Countdown)
            gf.textStyle.size = countdownNumberSize(frameSize.height());
        gf.textStyle.canvas = image.size();
        gf.textStyle.scale = double(image.height()) / frameSize.height();
        const TitleImage text = renderTitle(gf.textStyle);
        if (!text.image.isNull()) {
            QPainter p(&image);
            p.drawImage(text.rect.translated(0, gf.textOffsetY * image.height()), text.image);
        }
    }
    return image;
}

} // namespace kk::render
