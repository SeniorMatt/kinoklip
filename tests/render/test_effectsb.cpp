// SPDX-License-Identifier: GPL-3.0-or-later
// The second part of the 1.2 shader effects (docs/behavior-spec.md section 57): Sharpen Edges,
// Soften and Sharpen, Texture Pop, Detail Recovery, Deband, Dead Pixel Fixer, Beauty, Analog
// Damage, Film Damage, JPEG Damage, Watercolor, Stylize, Aperture Diffraction, Lens
// Reflections, Color Palette, 3D Keyer and Video Collage. Registry checks, neutral parameters,
// direction-of-change checks with numbers, determinism per frame, and a project round trip. GPU
// cases skip without OpenGL (these effects have no CPU version).
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "render/Compositor.h"
#include "render/GlEffects.h"

#include <QTemporaryDir>

#include <set>

using namespace kk;
using namespace kk::test;
using namespace kk::render;

namespace {

namespace g = generators;
namespace k = fxkeys;

constexpr int kW = 320, kH = 180;

ClipData generatorClip(TimelineData& tl, const QString& id, int64_t duration = 100)
{
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Generator;
    c.generator = id;
    c.title = EffectRegistry::instance().defaultParams(id);
    c.start = 0;
    c.duration = duration;
    return c;
}

using Values = std::initializer_list<std::pair<const QString, double>>;
using Statics = std::initializer_list<std::pair<const QString, QVariant>>;

EffectInstance effect(TimelineData& tl, const char* type, Values values = {}, Statics statics = {})
{
    EffectInstance e
        = EffectRegistry::instance().createEffect(QLatin1String(type), tl.allocateId());
    for (const auto& [key, v] : values)
        e.params.setBase(key, v);
    for (const auto& [key, v] : statics)
        e.params.setStatic(key, v);
    return e;
}

int maxDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
    }
    return worst;
}

/// Pixels whose grey value differs from @p b's by more than @p tolerance.
int countDifferent(const QImage& a, const QImage& b, int tolerance)
{
    int n = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            n += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y))) > tolerance ? 1 : 0;
    return n;
}

/// Mean absolute grey difference of rows [y0, y1).
double rowsDifference(const QImage& a, const QImage& b, int y0, int y1)
{
    double sum = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < a.width(); ++x)
            sum += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
    return sum / ((y1 - y0) * a.width());
}

double meanGray(const QImage& img)
{
    double sum = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            sum += qGray(img.pixel(x, y));
    return sum / (img.width() * img.height());
}

int uniqueGrays(const QImage& img, int y)
{
    std::set<int> v;
    for (int x = 0; x < img.width(); ++x)
        v.insert(qGray(img.pixel(x, y)));
    return int(v.size());
}

/// All new video effects: id, folder.
const std::pair<const char*, const char*> kEffects[] = {
    {fxid::SharpenEdges, effectgroups::Sharpen},
    {fxid::SoftenSharpen, effectgroups::Sharpen},
    {fxid::TexturePop, effectgroups::Texture},
    {fxid::DetailRecovery, effectgroups::Texture},
    {fxid::AnalogDamage, effectgroups::Texture},
    {fxid::FilmDamage, effectgroups::Texture},
    {fxid::JpegDamage, effectgroups::Texture},
    {fxid::Deband, effectgroups::Revival},
    {fxid::DeadPixelFixer, effectgroups::Revival},
    {fxid::Beauty, effectgroups::Refine},
    {fxid::Watercolor, effectgroups::Stylize},
    {fxid::Stylize, effectgroups::Stylize},
    {fxid::ApertureDiffraction, effectgroups::Light},
    {fxid::LensReflections, effectgroups::Light},
    {fxid::ColorPalette, effectgroups::Color},
    {fxid::Keyer3d, effectgroups::Key},
    {fxid::VideoCollage, effectgroups::Transform},
};

} // namespace

class TestEffectsB : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    QTemporaryDir m_dir;

    bool gpuReady() const { return m_gpu && m_gpu->isGpu(); }

    QImage render(const TimelineData& tl, int64_t frame = 0)
    {
        return m_gpu->renderImage(snap(tl), frame);
    }

    /// A Window clip (@p width x @p height px of @p inside on @p outside, centred at
    /// (@p cx, @p cy) from the middle) with @p effects, over black.
    QImage windowWith(const QVector<EffectInstance>& effects, int64_t frame = 0,
        const QColor& inside = QColor(160, 160, 160), const QColor& outside = QColor(100, 100, 100),
        double width = 120, double height = 80, double cx = 0, double cy = 0,
        const QString& shape = QStringLiteral("rectangle"))
    {
        TimelineData tl = emptyTimeline(kW, kH);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 100));
        ClipData win = generatorClip(tl, g::Window);
        win.title.setBase(g::Width, width);
        win.title.setBase(g::Height, height);
        win.title.setBase(g::CenterX, cx);
        win.title.setBase(g::CenterY, cy);
        win.title.setStatic(g::Shape, shape);
        win.title.setStatic(g::Color1, inside);
        win.title.setStatic(g::Color2, outside);
        win.effects = effects;
        addClip(tl, TrackType::Video, 1, std::move(win));
        return render(tl, frame);
    }

    /// A generator clip over black, with @p setup applied to its parameters.
    template<typename Setup>
    QImage generatorWith(
        const QString& id, const QVector<EffectInstance>& effects, Setup&& setup, int64_t frame = 0)
    {
        TimelineData tl = emptyTimeline(kW, kH);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 100));
        ClipData c = generatorClip(tl, id);
        setup(c);
        c.effects = effects;
        addClip(tl, TrackType::Video, 1, std::move(c));
        return render(tl, frame);
    }

    QImage barsWith(const QVector<EffectInstance>& effects, int64_t frame = 0)
    {
        return generatorWith(
            QStringLiteral("color_bars"), effects,
            [](ClipData& c) { c.title.setStatic(g::Style, QStringLiteral("full")); }, frame);
    }

    /// A noisy grey picture (the Noise generator, mono), with @p effects.
    QImage noiseWith(const QVector<EffectInstance>& effects, double amount = 12, int64_t frame = 0)
    {
        return generatorWith(
            g::Noise, effects,
            [&](ClipData& c) {
                c.title.setBase(g::Amount, amount);
                c.title.setBase(g::Grain, 1);
                c.title.setStatic(g::Monochrome, true);
                c.title.setStatic(g::Animated, false);
            },
            frame);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_gpu = std::make_unique<Compositor>();
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
    }
    void cleanupTestCase() { m_gpu.reset(); }

    // ---- registry ----

    void effectsAreRegistered()
    {
        const EffectRegistry& registry = EffectRegistry::instance();
        std::set<QString> gpuIds;
        for (const GlEffectFactory& f : builtinGlEffects())
            gpuIds.insert(QLatin1String(f.id));
        for (const auto& [id, group] : kEffects) {
            const EffectDescriptor* d = registry.find(QLatin1String(id));
            QVERIFY2(d, id);
            QCOMPARE(d->category, EffectCategory::VideoEffect);
            QCOMPARE(d->group, QLatin1String(group));
            QVERIFY(!d->name.isEmpty() && !d->description.isEmpty());
            QVERIFY2(gpuIds.count(d->id), id);
            QVERIFY2(!d->params.isEmpty(), id);
            const ParamSet defaults = registry.defaultParams(d->id);
            for (const ParamDescriptor& p : d->params) {
                if (p.isScalar()) {
                    QVERIFY2(p.def >= p.min && p.def <= p.max, qPrintable(d->id + p.key));
                    QCOMPARE(defaults.value(p.key, 0, -12345.0), p.def);
                } else {
                    QVERIFY2(defaults.statics.contains(p.key), qPrintable(d->id + p.key));
                    if (p.type == ParamType::Choice)
                        QVERIFY(p.choices.contains(p.defStatic.toString()));
                }
            }
        }
        // The names the library shows.
        const QStringList names {QStringLiteral("Sharpen Edges"),
            QStringLiteral("Soften and Sharpen"), QStringLiteral("Texture Pop"),
            QStringLiteral("Detail Recovery"), QStringLiteral("Analog Damage"),
            QStringLiteral("Film Damage"), QStringLiteral("JPEG Damage"), QStringLiteral("Deband"),
            QStringLiteral("Dead Pixel Fixer"), QStringLiteral("Beauty"),
            QStringLiteral("Watercolor"), QStringLiteral("Stylize"),
            QStringLiteral("Aperture Diffraction"), QStringLiteral("Lens Reflections"),
            QStringLiteral("Color Palette"), QStringLiteral("3D Keyer"),
            QStringLiteral("Video Collage")};
        int i = 0;
        for (const auto& [id, group] : kEffects) {
            Q_UNUSED(group);
            QCOMPARE(registry.find(QLatin1String(id))->name, names[i++]);
        }
        // New folders, with their names; Revival now has effects.
        const QStringList folders = registry.groups(EffectCategory::VideoEffect);
        for (const char* f : {effectgroups::Sharpen, effectgroups::Refine, effectgroups::Revival})
            QVERIFY(folders.contains(QLatin1String(f)));
        QCOMPARE(EffectRegistry::groupName(effectgroups::Sharpen), QStringLiteral("Sharpen"));
        QCOMPARE(EffectRegistry::groupName(effectgroups::Refine), QStringLiteral("Refine"));
    }

    // ---- neutral parameters ----

    void neutralParametersLeavePictureAlone()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QImage plain = barsWith({});
        TimelineData t = emptyTimeline();
        struct Row
        {
            const char* type;
            Values values;
        };
        const Row rows[] = {
            {fxid::SharpenEdges, {{k::Amount, 0.0}}},
            {fxid::SoftenSharpen, {{k::Soften, 0.0}, {k::Sharpen, 0.0}}},
            {fxid::TexturePop, {{k::Amount, 0.0}}},
            {fxid::DetailRecovery, {{k::Shadows, 0.0}, {k::Highlights, 0.0}, {k::Detail, 0.0}}},
            {fxid::Deband, {{k::Threshold, 0.0}}},
            {fxid::DeadPixelFixer, {}}, // nothing in colour bars is a single stuck pixel
            {fxid::Beauty, {{k::Smoothing, 0.0}}},
            {fxid::AnalogDamage,
                {{k::Jitter, 0.0}, {k::ColorBleed, 0.0}, {k::Noise, 0.0}, {k::Tracking, 0.0},
                    {k::ScanlineAmount, 0.0}}},
            {fxid::FilmDamage,
                {{k::Dirt, 0.0}, {k::Scratches, 0.0}, {k::Vignette, 0.0}, {k::Blur, 0.0},
                    {k::Weave, 0.0}, {k::FlickerAmount, 0.0}}},
            {fxid::JpegDamage, {{k::Quality, 100.0}}},
            {fxid::Stylize, {{k::Amount, 0.0}}},
            {fxid::ApertureDiffraction, {{k::Intensity, 0.0}}},
            {fxid::LensReflections, {{k::Intensity, 0.0}}},
            {fxid::ColorPalette, {{k::Opacity, 0.0}}},
            {fxid::VideoCollage, {{k::Columns, 1.0}, {k::Rows, 1.0}, {k::Spacing, 0.0}}},
        };
        for (const Row& r : rows) {
            const QImage img = barsWith({effect(t, r.type, r.values)});
            QVERIFY2(maxDifference(img, plain) <= 1, r.type);
        }
        // The 3D Keyer with no match leaves a picture whole: bars have no green near (0,0,0).
        const QImage keyed = barsWith({effect(t, fxid::Keyer3d,
            {{k::Tolerance, 0.0}, {k::Softness, 0.0}}, {{k::KeyColor, QColor(1, 2, 3)}})});
        QVERIFY(maxDifference(keyed, plain) <= 40); // only the exact black bar may go
        // Flat areas stay flat under the sharpening and repair effects.
        const QColor mid(120, 120, 120);
        const QImage flat = windowWith({}, 0, mid, mid);
        const Row flatRows[]
            = {{fxid::SharpenEdges, {}}, {fxid::SoftenSharpen, {}}, {fxid::TexturePop, {}},
                {fxid::DetailRecovery, {{k::Shadows, 0.0}, {k::Highlights, 0.0}}},
                {fxid::Deband, {}}, {fxid::DeadPixelFixer, {}}, {fxid::Beauty, {}}};
        for (const Row& r : flatRows)
            QVERIFY2(
                maxDifference(windowWith({effect(t, r.type, r.values)}, 0, mid, mid), flat) <= 3,
                r.type);
    }

    // ---- sharpen group ----

    void sharpenEdges()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage plain = windowWith({});
        const QImage sharp
            = windowWith({effect(t, fxid::SharpenEdges, {{k::Amount, 200.0}, {k::Radius, 3.0}})});
        // Grey 160 inside the window, 100 outside; its left edge is at x = 100.
        const int inside0 = qGray(plain.pixel(101, 90)), inside1 = qGray(sharp.pixel(101, 90));
        const int outside0 = qGray(plain.pixel(98, 90)), outside1 = qGray(sharp.pixel(98, 90));
        qInfo("sharpen edges: inside %d -> %d, outside %d -> %d", inside0, inside1, outside0,
            outside1);
        QVERIFY(inside1 > inside0 + 12);
        QVERIFY(outside1 < outside0 - 12);
        // The halo limit (25 % = 64 levels) caps the overshoot.
        QVERIFY(inside1 <= inside0 + 65);
        // Flat areas are untouched.
        QVERIFY(std::abs(qGray(sharp.pixel(160, 90)) - 160) <= 1);
        QVERIFY(std::abs(qGray(sharp.pixel(20, 20)) - 100) <= 1);
        // A step far below the edge threshold (6 levels = 2 %) is texture, not an edge.
        const QImage faint = windowWith({effect(t, fxid::SharpenEdges, {{k::Amount, 300.0}})}, 0,
            QColor(106, 106, 106), QColor(100, 100, 100));
        QVERIFY(
            maxDifference(faint, windowWith({}, 0, QColor(106, 106, 106), QColor(100, 100, 100)))
            <= 1);
        // A smaller halo limit caps harder.
        const QImage capped = windowWith(
            {effect(t, fxid::SharpenEdges, {{k::Amount, 200.0}, {k::HaloLimit, 5.0}})});
        QVERIFY(qGray(capped.pixel(101, 90)) <= inside0 + 14);
    }

    void softenAndSharpen()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        // Soften removes fine texture: the noise energy falls by more than half.
        const QImage noise = noiseWith({});
        const QImage soft = noiseWith({effect(t, fxid::SoftenSharpen,
            {{k::Soften, 100.0}, {k::Sharpen, 0.0}, {k::Threshold, 40.0}})});
        const double e0 = highFrequencyEnergy(noise), e1 = highFrequencyEnergy(soft);
        qInfo("soften: noise energy %.0f -> %.0f", e0, e1);
        QVERIFY(e0 > 0 && e1 < 0.5 * e0);
        // Sharpen overshoots the real edge.
        const QImage plain = windowWith({});
        const QImage sharp = windowWith({effect(t, fxid::SoftenSharpen,
            {{k::Soften, 0.0}, {k::Sharpen, 150.0}, {k::Radius, 4.0}, {k::Threshold, 2.0}})});
        QVERIFY(qGray(sharp.pixel(101, 90)) > qGray(plain.pixel(101, 90)) + 8);
        QVERIFY(qGray(sharp.pixel(98, 90)) < qGray(plain.pixel(98, 90)) - 8);
    }

    void texturePop()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage noise = noiseWith({});
        const QImage pop = noiseWith({effect(t, fxid::TexturePop, {{k::Amount, 200.0}})});
        const QImage flat
            = noiseWith({effect(t, fxid::TexturePop, {{k::Amount, -100.0}, {k::Radius, 3.0}})});
        const double e0 = highFrequencyEnergy(noise), e1 = highFrequencyEnergy(pop),
                     e2 = highFrequencyEnergy(flat);
        qInfo("texture pop: energy %.0f -> %.0f (pop), %.0f (negative)", e0, e1, e2);
        QVERIFY(e1 > 1.5 * e0);
        QVERIFY(e2 < 0.6 * e0);
        // Highlight protection: the same step pops less in the highlights than in the midtones.
        auto pops = [&](int hi, int lo) {
            const QImage a
                = windowWith({effect(t, fxid::TexturePop,
                                 {{k::Amount, 100.0}, {k::Highlights, 100.0}, {k::Radius, 4.0}})},
                    0, QColor(hi, hi, hi), QColor(lo, lo, lo));
            const QImage b = windowWith({}, 0, QColor(hi, hi, hi), QColor(lo, lo, lo));
            return qGray(a.pixel(101, 90)) - qGray(b.pixel(101, 90));
        };
        const int mid = pops(150, 100), bright = pops(250, 200);
        qInfo("texture pop: midtone lift %d, highlight lift %d", mid, bright);
        QVERIFY(mid > 8);
        QVERIFY(bright < mid - 4);
    }

    void detailRecovery()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        // A dark window on mid grey: shadows lifted, the mid grey far away stays.
        const QImage dark = windowWith({}, 0, QColor(30, 30, 30), QColor(128, 128, 128));
        const QImage lifted
            = windowWith({effect(t, fxid::DetailRecovery,
                             {{k::Shadows, 100.0}, {k::Highlights, 0.0}, {k::Detail, 0.0}})},
                0, QColor(30, 30, 30), QColor(128, 128, 128));
        qInfo("detail recovery: shadow %d -> %d", qGray(dark.pixel(160, 90)),
            qGray(lifted.pixel(160, 90)));
        QVERIFY(qGray(lifted.pixel(160, 90)) > qGray(dark.pixel(160, 90)) + 15);
        QVERIFY(std::abs(qGray(lifted.pixel(10, 10)) - 128) <= 2);
        // A bright window: highlights pulled down.
        const QImage bright = windowWith({}, 0, QColor(240, 240, 240), QColor(128, 128, 128));
        const QImage pulled
            = windowWith({effect(t, fxid::DetailRecovery,
                             {{k::Shadows, 0.0}, {k::Highlights, 100.0}, {k::Detail, 0.0}})},
                0, QColor(240, 240, 240), QColor(128, 128, 128));
        QVERIFY(qGray(pulled.pixel(160, 90)) < qGray(bright.pixel(160, 90)) - 10);
        // Detail alone sharpens the step without moving the flat tones.
        const QImage plain = windowWith({});
        const QImage detail = windowWith({effect(t, fxid::DetailRecovery,
            {{k::Shadows, 0.0}, {k::Highlights, 0.0}, {k::Detail, 100.0}, {k::Radius, 8.0}})});
        QVERIFY(qGray(detail.pixel(101, 90)) > qGray(plain.pixel(101, 90)) + 5);
        QVERIFY(qGray(detail.pixel(98, 90)) < qGray(plain.pixel(98, 90)) - 5);
        QVERIFY(std::abs(qGray(detail.pixel(160, 90)) - 160) <= 2);
    }

    // ---- revival ----

    void debandSmoothsSteps()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        auto ramp = [&](const QVector<EffectInstance>& fx, int64_t frame = 0) {
            return generatorWith(
                g::GreyScale, fx,
                [](ClipData& c) {
                    c.title.setBase(g::Steps, 16);
                    c.title.setStatic(g::Orientation, QStringLiteral("horizontal"));
                },
                frame);
        };
        auto largestStep = [](const QImage& img) {
            int worst = 0;
            for (int x = 8; x < img.width() - 8; ++x)
                worst = std::max(
                    worst, std::abs(qGray(img.pixel(x, 90)) - qGray(img.pixel(x + 1, 90))));
            return worst;
        };
        const QImage banded = ramp({});
        const QImage smooth = ramp(
            {effect(t, fxid::Deband, {{k::Threshold, 40.0}, {k::Radius, 20.0}, {k::Dither, 0.0}})});
        qInfo("deband: largest step %d -> %d", largestStep(banded), largestStep(smooth));
        QVERIFY(largestStep(banded) >= 14);
        QVERIFY(largestStep(smooth) <= 5);
        // The ramp still runs from dark to bright.
        QVERIFY(qGray(smooth.pixel(30, 90)) < 40 && qGray(smooth.pixel(290, 90)) > 215);
        // Dither: varies with the frame, repeats for the same frame, and is small.
        const QVector<EffectInstance> dithered = {
            effect(t, fxid::Deband, {{k::Threshold, 40.0}, {k::Radius, 20.0}, {k::Dither, 100.0}})};
        const QImage d0 = ramp(dithered, 0), d0b = ramp(dithered, 0), d1 = ramp(dithered, 1);
        QVERIFY(maxDifference(d0, d0b) == 0);
        QVERIFY(maxDifference(d0, d1) >= 1);
        QVERIFY(maxDifference(d0, smooth) >= 1 && maxDifference(d0, smooth) <= 4);
        // At the defaults a smooth (unbanded) ramp or gradient is left clean: at most one level
        // of change per pixel, no pattern.
        const QImage smoothRamp = generatorWith(g::GreyScale, {}, [](ClipData& c) {
            c.title.setBase(g::Steps, 0);
            c.title.setStatic(g::Orientation, QStringLiteral("horizontal"));
        });
        const QImage smoothRampFixed
            = generatorWith(g::GreyScale, {effect(t, fxid::Deband)}, [](ClipData& c) {
                  c.title.setBase(g::Steps, 0);
                  c.title.setStatic(g::Orientation, QStringLiteral("horizontal"));
              });
        QVERIFY(maxDifference(smoothRamp, smoothRampFixed) <= 1);
        const QImage gradient
            = generatorWith(QStringLiteral("four_color_gradient"), {}, [](ClipData&) { });
        const QImage gradientFixed = generatorWith(
            QStringLiteral("four_color_gradient"), {effect(t, fxid::Deband)}, [](ClipData&) { });
        QVERIFY(maxDifference(gradient, gradientFixed) <= 1);
        // A real edge (60 levels) is kept.
        const QImage edge = windowWith({effect(t, fxid::Deband, {{k::Threshold, 8.0}})});
        QVERIFY(qGray(edge.pixel(103, 90)) >= 158);
        QVERIFY(qGray(edge.pixel(97, 90)) <= 102);
    }

    void deadPixelFixer()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        // One white pixel at (160, 90) on grey 100: the window is 1 px, shifted half a pixel.
        auto pixelAt = [&](const QVector<EffectInstance>& fx, const QColor& inside,
                           const QColor& outside, double size = 1) {
            return windowWith(
                fx, 0, inside, outside, size, size, size == 1 ? 0.5 : 0, size == 1 ? 0.5 : 0);
        };
        const QImage stuck = pixelAt({}, Qt::white, QColor(100, 100, 100));
        QVERIFY2(qGray(stuck.pixel(160, 90)) >= 250, "the test picture has its bright pixel");
        const QImage fixed
            = pixelAt({effect(t, fxid::DeadPixelFixer)}, Qt::white, QColor(100, 100, 100));
        qInfo("dead pixel: %d -> %d", qGray(stuck.pixel(160, 90)), qGray(fixed.pixel(160, 90)));
        QVERIFY(qGray(fixed.pixel(160, 90)) <= 115);
        QVERIFY(std::abs(qGray(fixed.pixel(161, 90)) - 100) <= 1); // neighbours were never touched
        QVERIFY(std::abs(qGray(fixed.pixel(100, 20)) - 100) <= 1);
        // Bright Only leaves a dark defect, Dark Only leaves a bright one.
        const QImage dark = pixelAt({}, Qt::black, QColor(100, 100, 100));
        QCOMPARE(qGray(dark.pixel(160, 90)), 0);
        QVERIFY(
            qGray(pixelAt({effect(t, fxid::DeadPixelFixer, {}, {{k::Detect, fxchoice::Bright}})},
                Qt::black, QColor(100, 100, 100))
                    .pixel(160, 90))
            <= 1);
        QVERIFY(qGray(pixelAt({effect(t, fxid::DeadPixelFixer, {}, {{k::Detect, fxchoice::Dark}})},
                    Qt::black, QColor(100, 100, 100))
                        .pixel(160, 90))
            >= 85);
        QVERIFY(qGray(pixelAt({effect(t, fxid::DeadPixelFixer, {}, {{k::Detect, fxchoice::Dark}})},
                    Qt::white, QColor(100, 100, 100))
                        .pixel(160, 90))
            >= 250);
        // A two-pixel blob is not a single stuck pixel at Defect Size 1 ...
        const QImage blob
            = pixelAt({effect(t, fxid::DeadPixelFixer)}, Qt::white, QColor(100, 100, 100), 2);
        QVERIFY(qGray(blob.pixel(159, 89)) >= 250);
        // ... but is at Defect Size 3.
        const QImage blobFixed = pixelAt({effect(t, fxid::DeadPixelFixer, {{k::Size, 3.0}})},
            Qt::white, QColor(100, 100, 100), 2);
        QVERIFY(qGray(blobFixed.pixel(159, 89)) <= 130);
    }

    void beautySmoothsSkinKeepsEdges()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage noise = noiseWith({}, 3);
        const QImage smooth
            = noiseWith({effect(t, fxid::Beauty,
                            {{k::Smoothing, 100.0}, {k::DetailRecover, 0.0}, {k::Radius, 6.0}})},
                3);
        const double e0 = highFrequencyEnergy(noise), e1 = highFrequencyEnergy(smooth);
        qInfo("beauty: noise energy %.0f -> %.0f", e0, e1);
        QVERIFY(e1 < 0.35 * e0);
        // Detail Recover 100 % gives all the texture back.
        const QImage back = noiseWith(
            {effect(t, fxid::Beauty, {{k::Smoothing, 100.0}, {k::DetailRecover, 100.0}})}, 3);
        QVERIFY(maxDifference(back, noise) <= 2);
        // 50 % recover sits between.
        const QImage half = noiseWith(
            {effect(t, fxid::Beauty, {{k::Smoothing, 100.0}, {k::DetailRecover, 50.0}})}, 3);
        const double e2 = highFrequencyEnergy(half);
        QVERIFY(e2 > 1.5 * e1 && e2 < e0);
        // Edges survive: a 60 level step stays.
        const QImage edge
            = windowWith({effect(t, fxid::Beauty, {{k::Smoothing, 100.0}, {k::Radius, 8.0}})});
        QVERIFY(qGray(edge.pixel(101, 90)) >= 156);
        QVERIFY(qGray(edge.pixel(98, 90)) <= 104);
        // Skin Tones Only: textured skin colour is smoothed, textured blue is left alone.
        auto textured = [&](const QColor& a, const QColor& b, bool skinOnly) {
            QVector<EffectInstance> fx;
            fx.append(effect(t, fxid::FastNoise,
                {{k::Scale, 4.0}, {k::Detail, 1.0}, {k::Contrast, 100.0}, {k::Amount, 100.0}},
                {{k::ShadowColor, a}, {k::HighlightColor, b}, {k::Mode, fxchoice::Normal}}));
            if (skinOnly)
                fx.append(effect(t, fxid::Beauty, {{k::Smoothing, 100.0}, {k::DetailRecover, 0.0}},
                    {{k::SkinOnly, true}}));
            TimelineData tl = emptyTimeline(kW, kH);
            ClipData base = solidClip(tl, Qt::black, 0, 100);
            base.effects = fx;
            addClip(tl, TrackType::Video, 0, std::move(base));
            return render(tl);
        };
        const double skinBefore
            = highFrequencyEnergy(textured(QColor(205, 150, 120), QColor(225, 170, 140), false));
        const double skinAfter
            = highFrequencyEnergy(textured(QColor(205, 150, 120), QColor(225, 170, 140), true));
        const double blueBefore
            = highFrequencyEnergy(textured(QColor(30, 50, 190), QColor(50, 80, 220), false));
        const double blueAfter
            = highFrequencyEnergy(textured(QColor(30, 50, 190), QColor(50, 80, 220), true));
        qInfo("beauty skin only: skin %.0f -> %.0f, blue %.0f -> %.0f", skinBefore, skinAfter,
            blueBefore, blueAfter);
        QVERIFY(skinBefore > 0 && skinAfter < 0.6 * skinBefore);
        QVERIFY(blueAfter > 0.97 * blueBefore);
    }

    // ---- damage ----

    void analogDamage()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        auto only = [&](Values extra, int64_t frame = 0, Statics statics = {}) {
            Values base {{k::Jitter, 0.0}, {k::ColorBleed, 0.0}, {k::Noise, 0.0},
                {k::Tracking, 0.0}, {k::ScanlineAmount, 0.0}};
            EffectInstance e = effect(t, fxid::AnalogDamage, base, statics);
            for (const auto& [key, v] : extra)
                e.params.setBase(key, v);
            return QVector<EffectInstance> {e};
        };
        // Jitter: the left edge of the window (x = 100) lands in different places per line.
        auto edgeColumns = [](const QImage& img) {
            std::set<int> columns;
            for (int y = 55; y < 125; ++y) {
                for (int x = 60; x < 160; ++x) {
                    if (qGray(img.pixel(x, y)) > 130) {
                        columns.insert(x);
                        break;
                    }
                }
            }
            return columns;
        };
        const auto still = edgeColumns(windowWith({}));
        const auto shaken = edgeColumns(windowWith(only({{k::Jitter, 20.0}})));
        qInfo("analog: edge columns %d -> %d", int(still.size()), int(shaken.size()));
        QCOMPARE(int(still.size()), 1);
        QVERIFY(shaken.size() >= 4);
        QVERIFY(*shaken.rbegin() - *shaken.begin() >= 4);
        // Determinism: the same frame repeats, another frame or seed differs.
        const QImage a = windowWith(only({{k::Jitter, 20.0}, {k::Noise, 50.0}}), 5);
        QCOMPARE(maxDifference(a, windowWith(only({{k::Jitter, 20.0}, {k::Noise, 50.0}}), 5)), 0);
        QVERIFY(maxDifference(a, windowWith(only({{k::Jitter, 20.0}, {k::Noise, 50.0}}), 6)) > 5);
        QVERIFY(maxDifference(
                    a, windowWith(only({{k::Jitter, 20.0}, {k::Noise, 50.0}, {k::Seed, 2.0}}), 5))
            > 5);
        // Colour bleed: red inside the window smears to the right of its right edge (x = 220).
        const QImage red = windowWith({}, 0, QColor(220, 30, 30), QColor(110, 110, 110));
        const QImage bled = windowWith(
            only({{k::ColorBleed, 14.0}}), 0, QColor(220, 30, 30), QColor(110, 110, 110));
        const Px beyond = px(bled, 224, 90), beyond0 = px(red, 224, 90);
        qInfo("analog: bleed r-g beyond the edge %d -> %d", beyond0.r - beyond0.g,
            beyond.r - beyond.g);
        QVERIFY(beyond0.r - beyond0.g <= 1);
        QVERIFY(beyond.r - beyond.g >= 12);
        // ... but the brightness edge stays where it was.
        auto y709 = [](const QImage& img, int x, int y) {
            const Px p = px(img, x, y);
            return 0.2126 * p.r + 0.7152 * p.g + 0.0722 * p.b;
        };
        QVERIFY(std::abs(y709(bled, 224, 90) - y709(red, 224, 90)) <= 6);
        // Noise adds energy to a flat picture.
        const QImage flat = windowWith({}, 0, QColor(128, 128, 128), QColor(128, 128, 128));
        QCOMPARE(highFrequencyEnergy(flat), 0.0);
        QVERIFY(highFrequencyEnergy(windowWith(
                    only({{k::Noise, 60.0}}), 0, QColor(128, 128, 128), QColor(128, 128, 128)))
            > 20000);
        // Scanlines darken every other pair of lines.
        const QImage lines = windowWith(
            only({{k::ScanlineAmount, 100.0}}), 0, QColor(128, 128, 128), QColor(128, 128, 128));
        QVERIFY(std::abs(qGray(lines.pixel(50, 90)) - qGray(lines.pixel(50, 92))) >= 30);
        // The tracking band sits at one end: its lines are pulled sideways far more than the
        // others (bars have vertical edges to show it).
        const QImage bars = barsWith({});
        const QImage tracked = barsWith(only({{k::Jitter, 10.0}, {k::Tracking, 100.0}}));
        const double bottom = rowsDifference(bars, tracked, 140, 178);
        const double top = rowsDifference(bars, tracked, 2, 40);
        qInfo("analog: tracking error rows: top %.1f, bottom %.1f", top, bottom);
        QVERIFY(bottom > 2.0 * top);
    }

    void filmDamage()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        auto only = [&](Values extra) {
            Values base {{k::Dirt, 0.0}, {k::Scratches, 0.0}, {k::Vignette, 0.0}, {k::Blur, 0.0},
                {k::Weave, 0.0}, {k::FlickerAmount, 0.0}};
            EffectInstance e = effect(t, fxid::FilmDamage, base);
            for (const auto& [key, v] : extra)
                e.params.setBase(key, v);
            return QVector<EffectInstance> {e};
        };
        const QColor grey(128, 128, 128);
        auto flatWith = [&](Values extra, int64_t frame = 0) {
            return windowWith(only(extra), frame, grey, grey);
        };
        const QImage flat = flatWith({});
        QCOMPARE(maxDifference(flat, windowWith({}, 0, grey, grey)), 0);
        // Dirt: some pixels turn dark or bright, most stay; repeats per frame, changes with it.
        const QImage dirty = flatWith({{k::Dirt, 100.0}});
        const int specks = countDifferent(dirty, flat, 30);
        qInfo("film damage: %d dirty pixels", specks);
        QVERIFY(specks > 40 && specks < 6000);
        QCOMPARE(maxDifference(dirty, flatWith({{k::Dirt, 100.0}})), 0);
        QVERIFY(countDifferent(dirty, flatWith({{k::Dirt, 100.0}}, 4), 30) > 40);
        QVERIFY(countDifferent(dirty, flatWith({{k::Dirt, 100.0}}, 4), 0) > 0);
        // ... and with less dirt there are fewer specks.
        QVERIFY(countDifferent(flatWith({{k::Dirt, 20.0}}), flat, 30) < specks);
        // Vignette: the corners darken, the centre does not.
        const QImage vig = flatWith({{k::Vignette, 100.0}});
        QVERIFY(qGray(vig.pixel(3, 3)) < 128 - 50);
        QVERIFY(std::abs(qGray(vig.pixel(160, 90)) - 128) <= 2);
        // Scratches: bright vertical lines, kept for three frames, then new ones.
        auto scratchColumns = [&](const QImage& img) {
            int columns = 0;
            for (int x = 0; x < img.width(); ++x) {
                int tall = 0;
                for (int y = 0; y < img.height(); ++y)
                    tall += qGray(img.pixel(x, y)) > 170 ? 1 : 0;
                columns += tall > 40 ? 1 : 0;
            }
            return columns;
        };
        const QImage scratched = flatWith({{k::Scratches, 100.0}});
        qInfo("film damage: %d scratch columns", scratchColumns(scratched));
        QVERIFY(scratchColumns(scratched) >= 3);
        QCOMPARE(maxDifference(scratched, flatWith({{k::Scratches, 100.0}}, 2)), 0);
        QVERIFY(maxDifference(scratched, flatWith({{k::Scratches, 100.0}}, 3)) > 30);
        QCOMPARE(scratchColumns(flatWith({{k::Scratches, 0.0}})), 0);
        // Flicker: the brightness changes from frame to frame.
        double lo = 1000, hi = -1;
        for (int f = 0; f < 6; ++f) {
            const double m = meanGray(flatWith({{k::FlickerAmount, 50.0}}, f));
            lo = std::min(lo, m);
            hi = std::max(hi, m);
        }
        qInfo("film damage: flicker mean %.1f .. %.1f", lo, hi);
        QVERIFY(hi - lo > 8);
        // Gate weave: the picture shifts, differently per frame.
        auto edgeX = [](const QImage& img) {
            for (int x = 60; x < 160; ++x)
                if (qGray(img.pixel(x, 90)) > 130)
                    return x;
            return -1;
        };
        std::set<int> positions;
        for (int f = 0; f < 6; ++f)
            positions.insert(edgeX(windowWith(only({{k::Weave, 12.0}}), f)));
        QVERIFY(positions.size() >= 3);
        // Softness blurs the window edge.
        const QImage soft = windowWith(only({{k::Blur, 6.0}}));
        const int inside = qGray(soft.pixel(101, 90));
        QVERIFY(inside < 156 && inside > 100);
    }

    void jpegDamage()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        auto ramp = [&](const QVector<EffectInstance>& fx) {
            return generatorWith(g::GreyScale, fx, [](ClipData& c) {
                c.title.setBase(g::Steps, 0);
                c.title.setStatic(g::Orientation, QStringLiteral("horizontal"));
            });
        };
        const QImage smooth = ramp({});
        QVERIFY(uniqueGrays(smooth, 90) > 200);
        const QImage damaged = ramp({effect(t, fxid::JpegDamage, {{k::Quality, 10.0}})});
        qInfo("jpeg: distinct levels %d -> %d", uniqueGrays(smooth, 90), uniqueGrays(damaged, 90));
        QVERIFY(uniqueGrays(damaged, 90) <= 50);
        // Constant inside every 8 pixel block (the blocks are the grid of the picture).
        for (int block = 2; block < 36; ++block) {
            const int v = qGray(damaged.pixel(block * 8, 90));
            for (int x = block * 8 + 1; x < block * 8 + 8; ++x)
                QVERIFY2(
                    std::abs(qGray(damaged.pixel(x, 90)) - v) <= 1, qPrintable(QString::number(x)));
        }
        // Steps happen at block borders; the 16 pixel blocks are twice as wide.
        const QImage wide
            = ramp({effect(t, fxid::JpegDamage, {{k::Quality, 10.0}, {k::BlockSize, 16.0}})});
        for (int x = 33; x < 47; ++x)
            QVERIFY(std::abs(qGray(wide.pixel(x, 90)) - qGray(wide.pixel(32, 90))) <= 1);
        // Higher quality stays closer to the original.
        const QImage better = ramp({effect(t, fxid::JpegDamage, {{k::Quality, 80.0}})});
        QVERIFY(maxDifference(better, smooth) < maxDifference(damaged, smooth));
        QVERIFY(uniqueGrays(better, 90) > uniqueGrays(damaged, 90));
        // Colour turns blotchy: a saturated red/blue edge loses its sharp chroma.
        const QImage colorEdge = windowWith({effect(t, fxid::JpegDamage, {{k::Quality, 5.0}})}, 0,
            QColor(230, 60, 60), QColor(60, 60, 230));
        QVERIFY(
            maxDifference(colorEdge, windowWith({}, 0, QColor(230, 60, 60), QColor(60, 60, 230)))
            > 20);
    }

    // ---- paint ----

    void watercolor()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const Values calm {{k::EdgeDarkening, 0.0}, {k::Paper, 0.0}, {k::Saturation, 0.0}};
        // Flat washes: noise is painted over.
        const QImage noise = noiseWith({});
        const QImage painted = noiseWith({effect(t, fxid::Watercolor, calm)});
        const double e0 = highFrequencyEnergy(noise), e1 = highFrequencyEnergy(painted);
        qInfo("watercolor: noise energy %.0f -> %.0f", e0, e1);
        QVERIFY(e1 < 0.25 * e0);
        // Edges are kept (Kuwahara picks the calm side).
        const QImage edge = windowWith(
            {effect(t, fxid::Watercolor, calm)}, 0, QColor(200, 200, 200), QColor(60, 60, 60));
        QVERIFY(qGray(edge.pixel(103, 90)) >= 190);
        QVERIFY(qGray(edge.pixel(97, 90)) <= 70);
        // Edge darkening pools pigment along the edge.
        const QImage ink
            = windowWith({effect(t, fxid::Watercolor,
                             {{k::EdgeDarkening, 100.0}, {k::Paper, 0.0}, {k::Saturation, 0.0}})},
                0, QColor(200, 200, 200), QColor(60, 60, 60));
        qInfo("watercolor: edge pixel %d -> %d", qGray(edge.pixel(102, 90)),
            qGray(ink.pixel(102, 90)));
        QVERIFY(qGray(ink.pixel(102, 90)) < qGray(edge.pixel(102, 90)) - 10);
        QVERIFY(std::abs(qGray(ink.pixel(160, 90)) - 200) <= 3); // far from the edge
        // Paper texture: a flat picture gets grain; none with Paper 0.
        const QColor grey(128, 128, 128);
        QCOMPARE(
            highFrequencyEnergy(windowWith({effect(t, fxid::Watercolor, calm)}, 0, grey, grey)),
            0.0);
        QVERIFY(highFrequencyEnergy(windowWith(
                    {effect(t, fxid::Watercolor,
                        {{k::EdgeDarkening, 0.0}, {k::Paper, 100.0}, {k::Saturation, 0.0}})},
                    0, grey, grey))
            > 5000);
        // Saturation.
        const QColor dull(200, 120, 120);
        auto sat = [&](double v) {
            const QImage img
                = windowWith({effect(t, fxid::Watercolor,
                                 {{k::EdgeDarkening, 0.0}, {k::Paper, 0.0}, {k::Saturation, v}})},
                    0, dull, dull);
            const Px p = px(img, 160, 90);
            return p.r - p.g;
        };
        QVERIFY(sat(-100) <= 1);
        QVERIFY(sat(100) > sat(0) + 40);
    }

    void stylize()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        // Relief: a window lit from the left shows a bright left edge and a dark right edge,
        // and the opposite light angle swaps them; flat areas keep their colour.
        auto relief = [&](double angle) {
            return windowWith({effect(t, fxid::Stylize, {{k::Angle, angle}, {k::Size, 2.0}})});
        };
        const QImage plain = windowWith({});
        const QImage fromLeft = relief(0), fromRight = relief(180);
        qInfo("stylize relief: left edge %d / %d, right edge %d / %d",
            qGray(fromLeft.pixel(101, 90)), qGray(fromRight.pixel(101, 90)),
            qGray(fromLeft.pixel(218, 90)), qGray(fromRight.pixel(218, 90)));
        QVERIFY(qGray(fromLeft.pixel(101, 90)) > qGray(plain.pixel(101, 90)) + 20);
        QVERIFY(qGray(fromRight.pixel(101, 90)) < qGray(plain.pixel(101, 90)) - 20);
        QVERIFY(qGray(fromLeft.pixel(218, 90)) < qGray(plain.pixel(218, 90)) - 20);
        QVERIFY(qGray(fromRight.pixel(218, 90)) > qGray(plain.pixel(218, 90)) + 20);
        QVERIFY(std::abs(qGray(fromLeft.pixel(160, 90)) - 160) <= 2);
        QVERIFY(std::abs(qGray(fromLeft.pixel(20, 20)) - 100) <= 2);
        // Ink: two levels, black line on the edge.
        const QImage ink = windowWith(
            {effect(t, fxid::Stylize, {{k::Levels, 2.0}}, {{k::Style, fxchoice::Ink}})});
        QVERIFY(qGray(ink.pixel(100, 90)) < 50);
        QVERIFY(std::abs(qGray(ink.pixel(160, 90)) - 128) <= 3); // 160/255 -> level 1 of 2 -> 128
        // Cutout: flat colour at 4 levels (160/255 -> 0.75 = 191), no outline.
        const QImage cut = windowWith(
            {effect(t, fxid::Stylize, {{k::Levels, 4.0}}, {{k::Style, fxchoice::Cutout}})});
        QVERIFY(std::abs(qGray(cut.pixel(160, 90)) - 191) <= 3);
        QVERIFY(qGray(cut.pixel(100, 90)) > 40);
        // Strength 50 % is halfway between the picture and the cutout.
        const QImage half = windowWith({effect(t, fxid::Stylize,
            {{k::Levels, 4.0}, {k::Amount, 50.0}}, {{k::Style, fxchoice::Cutout}})});
        QVERIFY(std::abs(qGray(half.pixel(160, 90)) - 175) <= 3);
    }

    // ---- optics ----

    void apertureDiffraction()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        // A small white disc on black; four spikes along the axes.
        auto light = [&](Values values, Statics statics = {}, const QColor& core = Qt::white) {
            return windowWith({effect(t, fxid::ApertureDiffraction, values, statics)}, 0, core,
                Qt::black, 10, 10, 0, 0, QStringLiteral("ellipse"));
        };
        const QImage plain
            = windowWith({}, 0, Qt::white, Qt::black, 10, 10, 0, 0, QStringLiteral("ellipse"));
        const QImage star = light({{k::Points, 4.0}, {k::Length, 100.0}, {k::Threshold, 50.0}});
        const int onAxis = qGray(star.pixel(200, 90)), onAxisVert = qGray(star.pixel(160, 130));
        const int offAxis = qGray(star.pixel(188, 118));
        qInfo("aperture: on axis %d / %d, off axis %d", onAxis, onAxisVert, offAxis);
        QVERIFY(qGray(plain.pixel(200, 90)) == 0);
        QVERIFY(onAxis > 30 && onAxisVert > 30);
        QVERIFY(offAxis <= 3);
        // The spike fades out and ends at the length.
        QVERIFY(qGray(star.pixel(170, 90)) > onAxis);
        QVERIFY(qGray(star.pixel(290, 90)) <= 1);
        // Rotation by 45 degrees moves the spikes onto the diagonals.
        const QImage turned = light(
            {{k::Points, 4.0}, {k::Length, 100.0}, {k::Threshold, 50.0}, {k::Rotation, 45.0}});
        QVERIFY(qGray(turned.pixel(200, 90)) <= 3);
        QVERIFY(qGray(turned.pixel(188, 118)) > 30);
        // More spikes fill the in-between directions (8 spikes: the diagonal is one).
        QVERIFY(
            qGray(
                light({{k::Points, 8.0}, {k::Length, 100.0}, {k::Threshold, 50.0}}).pixel(188, 118))
            > 20);
        // Intensity.
        QVERIFY(qGray(light({{k::Points, 4.0}, {k::Length, 100.0}, {k::Threshold, 50.0},
                                {k::Intensity, 200.0}})
                        .pixel(200, 90))
            > onAxis + 15);
        // Threshold: a dim light (grey 200 = 78 %) makes spikes at 50 %, none at 90 %.
        const QColor dim(200, 200, 200);
        QVERIFY(qGray(light({{k::Points, 4.0}, {k::Threshold, 50.0}}, {}, dim).pixel(200, 90)) > 5);
        QVERIFY(
            qGray(light({{k::Points, 4.0}, {k::Threshold, 90.0}}, {}, dim).pixel(200, 90)) <= 1);
        // Tint colours the spikes.
        const Px red = px(light({{k::Points, 4.0}, {k::Length, 100.0}, {k::Threshold, 50.0}},
                              {{k::Color, QColor(255, 0, 0)}}),
            200, 90);
        QVERIFY(red.r > 30 && red.g <= 2 && red.b <= 2);
    }

    void lensReflections()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        // A white disc 80 px right of the centre: its reflection lies 80 px left of it.
        auto lit = [&](Values values, Statics statics = {}, const QColor& core = Qt::white) {
            return windowWith({effect(t, fxid::LensReflections, values, statics)}, 0, core,
                Qt::black, 20, 20, 80, 0, QStringLiteral("ellipse"));
        };
        const Values one {
            {k::Ghosts, 1.0}, {k::Threshold, 50.0}, {k::Softness, 4.0}, {k::Distance, 100.0}};
        const QImage ghost = lit(one, {{k::Color, QColor(255, 255, 255)}});
        qInfo("reflections: mirrored %d, elsewhere %d, light %d", qGray(ghost.pixel(80, 90)),
            qGray(ghost.pixel(80, 40)), qGray(ghost.pixel(240, 90)));
        QVERIFY(qGray(ghost.pixel(80, 90)) > 40);
        QVERIFY(qGray(ghost.pixel(80, 40)) <= 2);
        QVERIFY(qGray(ghost.pixel(240, 90)) >= 250); // the light itself is untouched
        // Spread 50 % puts it halfway: 40 px left of the centre.
        const QImage nearer
            = lit({{k::Ghosts, 1.0}, {k::Threshold, 50.0}, {k::Softness, 4.0}, {k::Distance, 50.0}},
                {{k::Color, QColor(255, 255, 255)}});
        QVERIFY(qGray(nearer.pixel(120, 90)) > 40);
        QVERIFY(qGray(nearer.pixel(80, 90)) <= 2);
        // The lens centre moves the mirror: centre at +40 puts it at +160 - ... = x 160.
        const QImage shifted = lit({{k::Ghosts, 1.0}, {k::Threshold, 50.0}, {k::Softness, 4.0},
                                       {k::Distance, 100.0}, {k::CenterX, 40.0}},
            {{k::Color, QColor(255, 255, 255)}});
        QVERIFY(qGray(shifted.pixel(160, 90)) > 40);
        QVERIFY(qGray(shifted.pixel(80, 90)) <= 2);
        // Intensity scales it; a light below the threshold makes none.
        const QImage strong = lit(
            {{k::Ghosts, 1.0}, {k::Threshold, 50.0}, {k::Softness, 4.0}, {k::Intensity, 160.0}},
            {{k::Color, QColor(255, 255, 255)}});
        QVERIFY(qGray(strong.pixel(80, 90)) > qGray(ghost.pixel(80, 90)) + 20);
        QVERIFY(qGray(lit({{k::Ghosts, 1.0}, {k::Threshold, 70.0}}, {}, QColor(100, 100, 100))
                        .pixel(80, 90))
            <= 1);
        // More reflections: a second one appears at another place on the axis (2 ghosts:
        // factors 0.5 and 1.0).
        const QImage two = lit(
            {{k::Ghosts, 2.0}, {k::Threshold, 50.0}, {k::Softness, 4.0}, {k::Distance, 100.0}},
            {{k::Color, QColor(255, 255, 255)}});
        QVERIFY(qGray(two.pixel(120, 90)) > 20 && qGray(two.pixel(80, 90)) > 20);
        // A mid-grey frame holds no light: nothing is added at the defaults.
        const QColor midGrey(128, 128, 128);
        QVERIFY(maxDifference(windowWith({effect(t, fxid::LensReflections)}, 0, midGrey, midGrey),
                    windowWith({}, 0, midGrey, midGrey))
            <= 1);
        // A broad bright area is not a light either.
        const QImage broad = windowWith(
            {effect(t, fxid::LensReflections)}, 0, Qt::white, QColor(40, 40, 40), 200, 140);
        QVERIFY(
            maxDifference(broad, windowWith({}, 0, Qt::white, QColor(40, 40, 40), 200, 140)) <= 2);
        // One dot: energy at the mirrored places, none along the mirrored frame border (the
        // two reflections at 50 % and 100 % of the spread put the border at x = 240 and x = 80,
        // y = 45 / 135 and 0 / 180).
        const QImage dot
            = lit({{k::Ghosts, 2.0}, {k::Distance, 100.0}}, {{k::Color, QColor(255, 255, 255)}});
        QVERIFY(qGray(dot.pixel(120, 90)) > 10 || qGray(dot.pixel(80, 90)) > 10);
        for (int y : {50, 55, 125, 130}) { // column x = 240 near the dot, away from it
            QVERIFY2(qGray(dot.pixel(240, y)) <= 2, qPrintable(QString::number(y)));
            QVERIFY2(qGray(dot.pixel(80, y)) <= 2, qPrintable(QString::number(y)));
        }
        for (int x : {100, 140, 200}) { // rows y = 45 and 135, the 50 % reflection's border
            QVERIFY2(qGray(dot.pixel(x, 45)) <= 2, qPrintable(QString::number(x)));
            QVERIFY2(qGray(dot.pixel(x, 135)) <= 2, qPrintable(QString::number(x)));
        }
        // The default tint is warm.
        const Px warm = px(lit(one), 80, 90);
        QVERIFY(warm.r > warm.b);
    }

    // ---- palette, keyer, collage ----

    void colorPalette()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QColor red(220, 30, 30), blue(30, 30, 220);
        // Red picture with a blue quarter on the right (x 240..320).
        auto picture = [&](const QVector<EffectInstance>& fx) {
            return windowWith(fx, 0, blue, red, 80, 180, 120, 0);
        };
        const QImage plain = picture({});
        const QImage strip = picture(
            {effect(t, fxid::ColorPalette, {{k::Count, 2.0}, {k::Size, 20.0}, {k::Gap, 0.0}})});
        // Bottom strip: two swatches, the biggest (red) first.
        const Px left = px(strip, 80, 175), right = px(strip, 240, 175);
        qInfo("palette: swatches %s %s", describe(left).constData(), describe(right).constData());
        QVERIFY(left.r > 190 && left.b < 60);
        QVERIFY(right.b > 190 && right.r < 60);
        // The rest of the picture is untouched.
        QVERIFY(maxDifference(strip.copy(0, 0, kW, 140), plain.copy(0, 0, kW, 140)) <= 1);
        // More swatches than colours repeat the picture's colours only.
        const QImage many = picture(
            {effect(t, fxid::ColorPalette, {{k::Count, 6.0}, {k::Size, 20.0}, {k::Gap, 0.0}})});
        for (int i = 0; i < 6; ++i) {
            const Px s = px(many, int((i + 0.5) * kW / 6), 175);
            const bool isRed = s.r > 190 && s.b < 60, isBlue = s.b > 190 && s.r < 60;
            QVERIFY2(isRed || isBlue, describe(s).constData());
        }
        // Gaps show the picture; opacity mixes it in; the strip can sit on top.
        const QImage gap = picture(
            {effect(t, fxid::ColorPalette, {{k::Count, 2.0}, {k::Size, 20.0}, {k::Gap, 20.0}})});
        QVERIFY(near(px(gap, 150, 175), px(plain, 150, 175).r, px(plain, 150, 175).g,
            px(plain, 150, 175).b, 255, 1));
        QVERIFY(near(px(gap, 140, 175), 220, 30, 30, 255, 40)); // swatch (red or its neighbour)
        const QImage half = picture({effect(t, fxid::ColorPalette,
            {{k::Count, 2.0}, {k::Size, 20.0}, {k::Gap, 0.0}, {k::Opacity, 50.0}})});
        QVERIFY(px(half, 165, 175).r > 80
            && px(half, 165, 175).r < 170); // blue swatch over red picture
        const QImage top = picture({effect(t, fxid::ColorPalette,
            {{k::Count, 2.0}, {k::Size, 20.0}, {k::Gap, 0.0}}, {{k::Position, fxchoice::PosTop}})});
        QVERIFY(near(px(top, 80, 5), 220, 30, 30, 255, 30));
        QVERIFY(maxDifference(top.copy(0, 40, kW, 140), plain.copy(0, 40, kW, 140)) <= 1);
        const QImage side = picture(
            {effect(t, fxid::ColorPalette, {{k::Count, 2.0}, {k::Size, 10.0}, {k::Gap, 0.0}},
                {{k::Position, fxchoice::PosLeft}})});
        QVERIFY(maxDifference(side.copy(40, 0, kW - 40, kH), plain.copy(40, 0, kW - 40, kH)) <= 1);
        QVERIFY(maxDifference(side.copy(0, 0, 20, kH), plain.copy(0, 0, 20, kH)) >= 0);
        // The same picture gives the same palette.
        QCOMPARE(maxDifference(strip,
                     picture({effect(t, fxid::ColorPalette,
                         {{k::Count, 2.0}, {k::Size, 20.0}, {k::Gap, 0.0}})})),
            0);
    }

    void keyer3d()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QColor green(0, 177, 64), red(200, 30, 30);
        auto keyed = [&](Values values, Statics statics = {},
                         const QColor& inside = QColor(0, 177, 64),
                         const QColor& outside = QColor(200, 30, 30)) {
            return windowWith({effect(t, fxid::Keyer3d, values, statics)}, 0, inside, outside);
        };
        // The green window disappears (black below), the red stays.
        const QImage img = keyed({});
        QVERIFY(near(px(img, 160, 90), 0, 0, 0, 255, 2));
        QVERIFY(near(px(img, 20, 20), 200, 30, 30, 255, 2));
        // Matte view: black where keyed, white where kept.
        const QImage matte = keyed({}, {{k::View, fxchoice::Matte}});
        QVERIFY(near(px(matte, 160, 90), 0, 0, 0, 255, 2));
        QVERIFY(near(px(matte, 20, 20), 255, 255, 255, 255, 2));
        // Invert swaps them.
        const QImage inverted = keyed({}, {{k::Invert, true}, {k::View, fxchoice::Matte}});
        QVERIFY(near(px(inverted, 160, 90), 255, 255, 255, 255, 2));
        QVERIFY(near(px(inverted, 20, 20), 0, 0, 0, 255, 2));
        // Similar greens go with it at Tolerance 25 %, a darker green not when brightness counts.
        QVERIFY(near(px(keyed({}, {}, QColor(20, 170, 80)), 160, 90), 0, 0, 0, 255, 2));
        const QColor darkGreen(0, 130, 47);
        QVERIFY(qGray(keyed({{k::LumaWeight, 0.0}}, {}, darkGreen).pixel(160, 90)) <= 2);
        QVERIFY(qGray(keyed({{k::LumaWeight, 100.0}}, {}, darkGreen).pixel(160, 90)) > 30);
        // Tolerance grows what goes.
        const QColor lime(60, 177, 100);
        QVERIFY(
            qGray(keyed({{k::Tolerance, 5.0}, {k::Softness, 0.0}}, {}, lime).pixel(160, 90)) > 30);
        QVERIFY(
            qGray(keyed({{k::Tolerance, 60.0}, {k::Softness, 0.0}}, {}, lime).pixel(160, 90)) <= 2);
        // Softness: a smooth ramp from keyed to kept, following smoothstep of the distance in
        // (Cb, Cr, sqrt(weight) * Y') space.
        auto space = [](const QColor& c, double weight) {
            const double r = c.redF(), gg = c.greenF(), b = c.blueF();
            const double y = 0.2126 * r + 0.7152 * gg + 0.0722 * b;
            return std::array<double, 3> {
                (b - y) / 1.8556, (r - y) / 1.5748, y * std::sqrt(weight)};
        };
        const auto a = space(green, 0.3), b = space(lime, 0.3);
        const double dist = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])
            + (a[2] - b[2]) * (a[2] - b[2]));
        const double tol = 0.05, soft = 0.25;
        const double u = std::clamp((dist - tol) / soft, 0.0, 1.0);
        const int expected = int(std::lround(255 * u * u * (3 - 2 * u)));
        const int measured = qGray(
            keyed({{k::Tolerance, 10.0}, {k::Softness, 50.0}}, {{k::View, fxchoice::Matte}}, lime)
                .pixel(160, 90));
        qInfo("3d keyer: distance %.3f, matte %d (expected %d)", dist, measured, expected);
        QVERIFY(expected > 20 && expected < 235);
        QVERIFY(std::abs(measured - expected) <= 6);
        // Colour 2 and 3 are used only when switched on.
        const QColor blue(0, 71, 187);
        QVERIFY(qGray(keyed({}, {}, blue).pixel(160, 90)) > 20);
        QVERIFY(qGray(keyed({}, {{k::UseColor2, true}}, blue).pixel(160, 90)) <= 2);
        QVERIFY(qGray(keyed({}, {{k::UseColor3, true}}, QColor(220, 40, 40)).pixel(160, 90)) <= 2);
        QVERIFY(qGray(keyed({}, {}, QColor(220, 40, 40)).pixel(160, 90)) > 20);
        // Clip White at 50 % keeps only what is more than half kept (a half keyed edge goes).
        QVERIFY(qGray(keyed({{k::Tolerance, 10.0}, {k::Softness, 50.0}, {k::ClipWhite, 50.0}},
                    {{k::View, fxchoice::Matte}}, lime)
                        .pixel(160, 90))
            >= measured);
    }

    void videoCollage()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage bars = barsWith({});
        const QImage grid = barsWith({effect(
            t, fxid::VideoCollage, {{k::Columns, 2.0}, {k::Rows, 2.0}, {k::Spacing, 0.0}})});
        // Each tile shows the whole picture at half size.
        int checked = 0;
        for (int x : {10, 50, 90, 130, 170, 250}) {
            for (int y : {10, 25, 100, 120}) {
                const Px a = px(grid, x, y), b = px(bars, (2 * x) % kW, (2 * y) % kH);
                QVERIFY2(near(a, b.r, b.g, b.b, 255, 4),
                    qPrintable(QStringLiteral("%1,%2").arg(x).arg(y)));
                ++checked;
            }
        }
        QCOMPARE(checked, 24);
        // Spacing opens gaps of the background (black here), the tiles keep their middle.
        const QImage spaced = barsWith({effect(t, fxid::VideoCollage,
            {{k::Columns, 2.0}, {k::Rows, 2.0}, {k::Spacing, 20.0}, {k::ShadowOpacity, 0.0}})});
        QVERIFY(near(px(spaced, 160, 90), 0, 0, 0, 255, 3));
        QVERIFY(near(px(spaced, 160, 20), 0, 0, 0, 255, 3));
        QVERIFY(qGray(spaced.pixel(60, 45)) > 20);
        // A background colour at full opacity fills the gaps with it.
        const QImage tinted = barsWith({effect(t, fxid::VideoCollage,
            {{k::Columns, 2.0}, {k::Rows, 2.0}, {k::Spacing, 20.0}, {k::ShadowOpacity, 0.0},
                {k::Opacity, 100.0}},
            {{k::BackgroundColor, QColor(10, 200, 10)}})});
        QVERIFY(near(px(tinted, 160, 90), 10, 200, 10, 255, 3));
        // The shadow falls down and to the right of a tile.
        const QImage shadowed = barsWith({effect(t, fxid::VideoCollage,
            {{k::Columns, 2.0}, {k::Rows, 2.0}, {k::Spacing, 40.0}, {k::ShadowOpacity, 100.0},
                {k::Opacity, 100.0}},
            {{k::BackgroundColor, QColor(255, 255, 255)}})});
        qInfo("collage shadow: right of tile %d, left of tile %d", qGray(shadowed.pixel(143, 45)),
            qGray(shadowed.pixel(17, 45)));
        QVERIFY(qGray(shadowed.pixel(143, 45)) < qGray(shadowed.pixel(17, 45)) - 80);
        // Corner radius cuts the corners of the tiles.
        const QImage rounded = barsWith({effect(t, fxid::VideoCollage,
            {{k::Columns, 1.0}, {k::Rows, 1.0}, {k::Spacing, 0.0}, {k::Radius, 40.0}})});
        QVERIFY(near(px(rounded, 1, 1), 0, 0, 0, 255, 3));
        qInfo("collage: rounded middle differs by %d",
            maxDifference(rounded.copy(60, 60, 200, 60), bars.copy(60, 60, 200, 60)));
        QVERIFY(maxDifference(rounded.copy(60, 60, 200, 60), bars.copy(60, 60, 200, 60)) <= 1);
        // Tile fit in a wide tile (1 column, 2 rows: 320 x 90): Stretch squashes, Fill crops
        // and covers it, Fit keeps the whole picture and leaves the sides empty.
        auto wide = [&](const QString& fit) {
            return barsWith({effect(t, fxid::VideoCollage,
                {{k::Columns, 1.0}, {k::Rows, 2.0}, {k::Spacing, 0.0}}, {{k::Fit, fit}})});
        };
        QVERIFY(qGray(wide(fxchoice::Stretch).pixel(5, 45)) > 20);
        QVERIFY(qGray(wide(fxchoice::Fill).pixel(5, 45)) > 20);
        QVERIFY(qGray(wide(fxchoice::FitInside).pixel(5, 45)) <= 2);
        QVERIFY(qGray(wide(fxchoice::FitInside).pixel(160, 20)) > 20);
    }

    // ---- project ----

    void projectRoundTrip()
    {
        Project project;
        TimelineData& tl = project.timeline()->mutableData();
        tl = emptyTimeline();
        ClipData fx = solidClip(tl, Qt::green, 0, 50);
        for (const auto& [id, group] : kEffects) {
            Q_UNUSED(group);
            fx.effects.append(effect(tl, id));
        }
        fx.effects[0].params.values[k::Amount].setKey(0, 10.0);
        fx.effects[0].params.values[k::Amount].setKey(20, 70.0);
        fx.effects[5].params.setBase(k::Seed, 77);
        fx.effects[12].params.setBase(k::Points, 9);
        fx.effects[15].params.setStatic(k::UseColor2, true);
        fx.effects[15].params.setStatic(k::Color2, QColor(1, 2, 3));
        fx.effects[11].params.setStatic(k::Style, fxchoice::Ink);
        fx.effects[8].params.setStatic(k::Detect, fxchoice::Dark);
        addClip(tl, TrackType::Video, 1, fx);
        project.timeline()->notifyChanged();
        const QString path = m_dir.filePath(QStringLiteral("effectsb.kinoklip"));
        QString error;
        QVERIFY2(project.save(path, &error), qPrintable(error));
        Project loaded;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const ClipData& back = loaded.timeline()->data().videoTracks[1].clips[0];
        QCOMPARE(back.effects.size(), fx.effects.size());
        for (int i = 0; i < fx.effects.size(); ++i) {
            QCOMPARE(back.effects[i].type, fx.effects[i].type);
            QCOMPARE(back.effects[i].params.statics, fx.effects[i].params.statics);
            for (const QString& key : fx.effects[i].params.values.keys())
                QCOMPARE(back.effects[i].params.value(key, 15, -1),
                    fx.effects[i].params.value(key, 15, -1));
        }
        QVERIFY(back.effects[0].params.values.value(k::Amount).isAnimated());
        QCOMPARE(back.effects[5].params.value(k::Seed, 0, -1), 77.0);
        QCOMPARE(back.effects[11].params.staticValue(k::Style).toString(), fxchoice::Ink);
        QCOMPARE(back.effects[15].params.staticValue(k::Color2).value<QColor>(), QColor(1, 2, 3));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestEffectsB>(argc, argv);
}

#include "test_effectsb.moc"
