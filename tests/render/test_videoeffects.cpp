// SPDX-License-Identifier: GPL-3.0-or-later
// The v4 video effects (docs/behavior-spec.md section 40): descriptor defaults and library
// folders, the CPU reference maths (render/EffectMath.h), GPU against the CPU reference for
// every pointwise effect, analytic checks of the spatial effects, the chroma key on a
// generated green-screen frame (8 and 10 bit, shrink / grow, edge blur, garbage matte), the
// CPU fallback and a Chroma Key export end to end. GPU cases skip without OpenGL.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Mask.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "export/ExportJob.h"
#include "media/VideoDecoder.h"
#include "render/Compositor.h"
#include "render/EffectMath.h"
#include "render/GlEffects.h"

#include <QElapsedTimer>
#include <QTemporaryDir>

#include <set>

using namespace kk;
using namespace kk::test;
using namespace kk::render;

namespace {

namespace k = fxkeys;

/// Every effect id added in v4 with its library folder.
const std::pair<const char*, const char*> kNewEffects[] = {
    {fxid::ChromaKey, effectgroups::Key},
    {fxid::LumaKey, effectgroups::Key},
    {fxid::HslKey, effectgroups::Key},
    {fxid::Exposure, effectgroups::Color},
    {fxid::TemperatureTint, effectgroups::Color},
    {fxid::Levels, effectgroups::Color},
    {fxid::Invert, effectgroups::Color},
    {fxid::BlackWhite, effectgroups::Color},
    {fxid::Posterize, effectgroups::Color},
    {fxid::ColorBalance, effectgroups::Color},
    {fxid::BoxBlur, effectgroups::Blur},
    {fxid::DirectionalBlur, effectgroups::Blur},
    {fxid::RadialBlur, effectgroups::Blur},
    {fxid::ZoomBlur, effectgroups::Blur},
    {fxid::Glow, effectgroups::Light},
    {fxid::FilmGrain, effectgroups::Stylize},
    {fxid::ChromaticAberration, effectgroups::Stylize},
    {fxid::EdgeDetect, effectgroups::Stylize},
    {fxid::Emboss, effectgroups::Stylize},
    {fxid::Stroke, effectgroups::Stylize},
    {fxid::LensDistortion, effectgroups::Distort},
    {fxid::Ripple, effectgroups::Distort},
    {fxid::Wave, effectgroups::Distort},
    {fxid::FeatheredCrop, effectgroups::Transform},
};

fxmath::Rgb rgb(const QColor& c)
{
    return {float(c.redF()), float(c.greenF()), float(c.blueF())};
}

/// An effect instance of @p type with default parameters plus @p values / @p statics.
EffectInstance effect(TimelineData& tl, const char* type,
    std::initializer_list<std::pair<const QString, double>> values = {},
    std::initializer_list<std::pair<const QString, QVariant>> statics = {})
{
    EffectInstance e
        = EffectRegistry::instance().createEffect(QLatin1String(type), tl.allocateId());
    for (const auto& [key, v] : values)
        e.params.setBase(key, v);
    for (const auto& [key, v] : statics)
        e.params.setStatic(key, v);
    return e;
}

/// Colours the pointwise tests run over (saturated, skin, greys, screen colours).
const QColor kSwatches[] = {QColor(200, 100, 50), QColor(230, 166, 128), QColor(30, 200, 90),
    QColor(0, 177, 64), QColor(0, 71, 187), QColor(128, 128, 128), QColor(20, 20, 20),
    QColor(240, 240, 240), QColor(90, 160, 220)};

int to8(float v)
{
    return int(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
}

/// Generates a 160 x 90 green-screen frame: screen green with a skin-tone box (40..119 x
/// 20..69) and a greenish box in the soft ramp (8..27 x 8..27), FFV1 in @p pixfmt, BT.709
/// limited range, tagged.
bool makeGreenScreen(const QString& path, const char* pixfmt)
{
    if (QFile::exists(path))
        return true;
    const QString filter = QStringLiteral("color=c=0x00B140:s=160x90:d=0.4:r=25,format=rgb24,"
                                          "drawbox=x=40:y=20:w=80:h=50:color=0xE6A680:t=fill,"
                                          "drawbox=x=8:y=8:w=20:h=20:color=0x40A050:t=fill,"
                                          "scale=out_color_matrix=bt709:out_range=tv,format=%1")
                               .arg(QLatin1String(pixfmt));
    QProcess ffmpeg;
    ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
    ffmpeg.start(QStringLiteral("ffmpeg"),
        {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), filter, QStringLiteral("-c:v"), QStringLiteral("ffv1"),
            QStringLiteral("-color_range"), QStringLiteral("tv"), QStringLiteral("-colorspace"),
            QStringLiteral("bt709"), QStringLiteral("-color_primaries"), QStringLiteral("bt709"),
            QStringLiteral("-color_trc"), QStringLiteral("bt709"), path});
    return ffmpeg.waitForFinished(60000) && ffmpeg.exitCode() == 0;
}

} // namespace

class TestVideoEffects : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    QTemporaryDir m_dir;
    QString m_screen8, m_screen10, m_screenError;

    QImage render(Compositor& c, const TimelineData& tl, int64_t frame = 0)
    {
        return c.renderImage(snap(tl), frame);
    }

    std::shared_ptr<MediaPathMap> media(const QString& path)
    {
        auto map = std::make_shared<MediaPathMap>();
        map->insert(QStringLiteral("gs"), path);
        return map;
    }

    /// V1: a red background solid; V2: the green-screen clip with @p effects (and @p masks).
    TimelineData greenScreenTimeline(const std::function<void(TimelineData&, ClipData&)>& setup)
    {
        TimelineData tl = emptyTimeline(160, 90);
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(220, 0, 0), 0, 10));
        ClipData fg = mediaClip(tl, QStringLiteral("gs"), 0, 10, 0, 400'000);
        setup(tl, fg);
        addClip(tl, TrackType::Video, 1, std::move(fg));
        return tl;
    }

    bool gpuReady() const { return m_gpu && m_gpu->isGpu(); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
        m_screen8 = m_dir.filePath(QStringLiteral("greenscreen8.mkv"));
        m_screen10 = m_dir.filePath(QStringLiteral("greenscreen10.mkv"));
        if (!makeGreenScreen(m_screen8, "yuv444p") || !makeGreenScreen(m_screen10, "yuv444p10le"))
            m_screenError = QStringLiteral("ffmpeg could not generate the green-screen frames");
    }

    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    // ---- Descriptors ----

    void descriptorsAndDefaults()
    {
        const EffectRegistry& registry = EffectRegistry::instance();
        std::set<QString> gpuIds;
        for (const GlEffectFactory& f : builtinGlEffects())
            gpuIds.insert(QLatin1String(f.id));
        for (const auto& [id, group] : kNewEffects) {
            const EffectDescriptor* d = registry.find(QLatin1String(id));
            QVERIFY2(d, id);
            QCOMPARE(d->category, EffectCategory::VideoEffect);
            QCOMPARE(d->group, QLatin1String(group));
            QVERIFY(!d->name.isEmpty() && !d->description.isEmpty());
            QVERIFY2(gpuIds.count(d->id), id); // every effect has a GPU implementation
            const ParamSet defaults = registry.defaultParams(d->id);
            for (const ParamDescriptor& p : d->params) {
                if (p.isScalar()) {
                    QVERIFY2(p.def >= p.min && p.def <= p.max, qPrintable(d->id + p.key));
                    QVERIFY2(p.animatable, qPrintable(d->id + p.key)); // keyframable
                    QCOMPARE(defaults.value(p.key, 0, -12345.0), p.def);
                } else {
                    QVERIFY(defaults.statics.contains(p.key));
                    if (p.type == ParamType::Choice)
                        QVERIFY(p.choices.contains(p.defStatic.toString()));
                }
            }
        }
        // Every video effect (old and new) sits in a library folder; folders sort by name.
        for (const EffectDescriptor* d : registry.byCategory(EffectCategory::VideoEffect))
            QVERIFY2(!d->group.isEmpty(), qPrintable(d->id));
        QCOMPARE(registry.groups(EffectCategory::VideoEffect),
            QStringList({QStringLiteral("blur"), QStringLiteral("color"), QStringLiteral("distort"),
                QStringLiteral("key"), QStringLiteral("light"), QStringLiteral("stylize"),
                QStringLiteral("time"), QStringLiteral("transform")}));
        QCOMPARE(EffectRegistry::groupName(QStringLiteral("key")), QStringLiteral("Key"));
        // The chroma key's colour has the viewer eyedropper.
        const ParamDescriptor* keyColor = registry.find(fxid::ChromaKey)->param(k::KeyColor);
        QVERIFY(keyColor && keyColor->viewerPicker && keyColor->type == ParamType::Color);
        QCOMPARE(registry.find(fxid::ChromaKey)->param(k::Screen)->defStatic.toString(),
            fxchoice::Green);
    }

    void projectRoundTrip()
    {
        Project project;
        TimelineData& tl = project.timeline()->mutableData();
        tl = emptyTimeline();
        ClipData clip = solidClip(tl, Qt::green, 0, 50);
        for (const auto& [id, group] : kNewEffects)
            clip.effects.append(effect(tl, id));
        // Non-default values, a keyframe and statics survive too.
        clip.effects[0].params.setStatic(k::Screen, fxchoice::Custom);
        clip.effects[0].params.setStatic(k::KeyColor, QColor(10, 200, 30));
        clip.effects[0].params.setStatic(k::View, fxchoice::Matte);
        clip.effects[0].params.values[k::Tolerance].setKey(0, 10.0);
        clip.effects[0].params.values[k::Tolerance].setKey(20, 60.0);
        addClip(tl, TrackType::Video, 0, clip);
        project.timeline()->notifyChanged();
        const QString path = m_dir.filePath(QStringLiteral("effects.kinoklip"));
        QString error;
        QVERIFY2(project.save(path, &error), qPrintable(error));
        Project loaded;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const ClipData& back = loaded.timeline()->data().videoTracks[0].clips[0];
        QCOMPARE(back.effects.size(), clip.effects.size());
        for (int i = 0; i < clip.effects.size(); ++i) {
            QCOMPARE(back.effects[i].type, clip.effects[i].type);
            QCOMPARE(back.effects[i].params.statics.keys(), clip.effects[i].params.statics.keys());
            for (const QString& key : clip.effects[i].params.values.keys())
                QCOMPARE(back.effects[i].params.value(key, 15, -1),
                    clip.effects[i].params.value(key, 15, -1));
        }
        QCOMPARE(
            back.effects[0].params.statics.value(k::KeyColor).value<QColor>(), QColor(10, 200, 30));
        QCOMPARE(back.effects[0].params.statics.value(k::View).toString(), fxchoice::Matte);
        QVERIFY(back.effects[0].params.values.value(k::Tolerance).isAnimated());
    }

    // ---- CPU reference maths ----

    void chromaKeyMaths()
    {
        ParamSet p = EffectRegistry::instance().defaultParams(fxid::ChromaKey);
        fxmath::ChromaKeyParams green = fxmath::chromaKeyParams(p, 0);
        // Pure screen colours and their brighter / darker / purer variants are transparent.
        QCOMPARE(fxmath::chromaKeyMatte(green, fxmath::screenPresetColor(false)), 0.f);
        // A shadow (60 %) and a lit part (125 %) of the screen are keyed as well.
        const fxmath::Rgb screen = fxmath::screenPresetColor(false);
        QCOMPARE(fxmath::chromaKeyMatte(green, {0.f, screen.g * 0.6f, screen.b * 0.6f}), 0.f);
        QCOMPARE(fxmath::chromaKeyMatte(green, {0.f, screen.g * 1.25f, screen.b * 1.25f}), 0.f);
        // Skin, greys, white, black, red and blue stay opaque.
        for (const fxmath::Rgb c : {fxmath::Rgb {0.9f, 0.65f, 0.5f}, fxmath::Rgb {0.5f, 0.5f, 0.5f},
                 fxmath::Rgb {1.f, 1.f, 1.f}, fxmath::Rgb {0.f, 0.f, 0.f},
                 fxmath::Rgb {0.86f, 0.f, 0.f}, fxmath::Rgb {0.1f, 0.2f, 0.8f}})
            QCOMPARE(fxmath::chromaKeyMatte(green, c), 1.f);
        // A greenish colour falls in the soft ramp.
        const float partial = fxmath::chromaKeyMatte(green, {0.25f, 0.63f, 0.31f});
        QVERIFY2(partial > 0.2f && partial < 0.99f, QByteArray::number(partial));

        // Blue preset: blue screen out, green stays.
        p.setStatic(k::Screen, fxchoice::Blue);
        const fxmath::ChromaKeyParams blue = fxmath::chromaKeyParams(p, 0);
        QCOMPARE(blue.spillChannel, 2);
        QCOMPARE(fxmath::chromaKeyMatte(blue, fxmath::screenPresetColor(true)), 0.f);
        QCOMPARE(fxmath::chromaKeyMatte(blue, fxmath::screenPresetColor(false)), 1.f);
        // Custom colour: the Key Color parameter; spill channel follows it.
        p.setStatic(k::Screen, fxchoice::Custom);
        p.setStatic(k::KeyColor, QColor(200, 30, 40));
        const fxmath::ChromaKeyParams red = fxmath::chromaKeyParams(p, 0);
        QCOMPARE(fxmath::chromaKeyMatte(red, {200 / 255.f, 30 / 255.f, 40 / 255.f}), 0.f);

        // Tolerance / softness widen the key; clip black / white and invert.
        fxmath::ChromaKeyParams wide = green;
        wide.tolerance = 0.6f;
        QVERIFY(fxmath::chromaKeyMatte(wide, {0.25f, 0.63f, 0.31f}) < partial);
        fxmath::ChromaKeyParams clipped = green;
        clipped.clipWhite = 0.5f;
        QVERIFY(fxmath::chromaKeyMatte(clipped, {0.25f, 0.63f, 0.31f})
            >= std::min(1.f, partial * 2 - 0.01f));
        clipped = green;
        clipped.clipBlack = 0.99f;
        QCOMPARE(fxmath::chromaKeyMatte(clipped, {0.25f, 0.63f, 0.31f}), 0.f);
        fxmath::ChromaKeyParams inverted = green;
        inverted.invert = true;
        QCOMPARE(fxmath::chromaKeyMatte(inverted, screen), 1.f);

        // Spill: green on a skin edge is limited to the mean / maximum of red and blue.
        const fxmath::Rgb spilled {0.7f, 0.8f, 0.5f};
        fxmath::ChromaKeyParams full = green;
        full.spillAmount = 1.f;
        QCOMPARE(fxmath::despill(full, spilled).g, 0.6f);
        full.spillMaximum = true;
        QCOMPARE(fxmath::despill(full, spilled).g, 0.7f);
        QCOMPARE(fxmath::despill(full, {0.9f, 0.6f, 0.5f}).g, 0.6f); // no spill: unchanged
    }

    void lumaAndHslKeyMaths()
    {
        ParamSet p = EffectRegistry::instance().defaultParams(fxid::LumaKey);
        const fxmath::LumaKeyParams luma = fxmath::lumaKeyParams(p, 0);
        QCOMPARE(fxmath::lumaKeyMatte(luma, {0.f, 0.f, 0.f}), 0.f); // below Low 20 %
        QCOMPARE(fxmath::lumaKeyMatte(luma, {0.6f, 0.6f, 0.6f}), 1.f);
        QCOMPARE(fxmath::lumaKeyMatte(luma, {1.f, 1.f, 1.f}), 1.f);
        const float edge = fxmath::lumaKeyMatte(luma, {0.15f, 0.15f, 0.15f});
        QVERIFY(edge > 0.f && edge < 1.f);
        fxmath::LumaKeyParams inv = luma;
        inv.invert = true;
        QCOMPARE(fxmath::lumaKeyMatte(inv, {0.f, 0.f, 0.f}), 1.f);

        float h = 0, s = 0, l = 0;
        fxmath::toHsl({0.f, 1.f, 0.f}, &h, &s, &l);
        QCOMPARE(h, 120.f);
        QCOMPARE(s, 1.f);
        QCOMPARE(l, 0.5f);
        const fxmath::HslKeyParams hsl
            = fxmath::hslKeyParams(EffectRegistry::instance().defaultParams(fxid::HslKey), 0);
        QCOMPARE(fxmath::hslKeyMatte(hsl, {0.1f, 0.8f, 0.2f}), 0.f); // green: keyed out
        QCOMPARE(fxmath::hslKeyMatte(hsl, {0.9f, 0.2f, 0.2f}), 1.f); // red: kept
        QCOMPARE(fxmath::hslKeyMatte(hsl, {0.5f, 0.5f, 0.5f}), 1.f); // grey: no saturation
    }

    void colorMaths()
    {
        const fxmath::Rgb c {0.5f, 0.25f, 0.75f};
        ParamSet p;
        p.setBase(k::Exposure, 1.0);
        const fxmath::Rgb brighter = fxmath::exposure(fxmath::exposureParams(p, 0), c);
        QVERIFY(brighter.r > c.r && brighter.g > c.g);
        QCOMPARE(fxmath::invert(1.f, c).r, 0.5f);
        QCOMPARE(fxmath::invert(1.f, c).g, 0.75f);
        QCOMPARE(fxmath::posterize(2, {0.4f, 0.6f, 1.f}).r, 0.f);
        QCOMPARE(fxmath::posterize(2, {0.4f, 0.6f, 1.f}).g, 1.f);
        ParamSet lv;
        lv.setBase(k::InBlack, 51);
        lv.setBase(k::InWhite, 204);
        const fxmath::Rgb stretched
            = fxmath::levels(fxmath::levelsParams(lv, 0), {0.2f, 0.8f, 0.5f});
        QVERIFY(std::abs(stretched.r) < 1e-4f && std::abs(stretched.g - 1.f) < 1e-4f);
        // Temperature keeps the brightness of a grey, warms it.
        ParamSet warm;
        warm.setBase(k::Temperature, 60);
        const fxmath::Rgb w
            = fxmath::linearGains(fxmath::temperatureTintParams(warm, 0), {0.5f, 0.5f, 0.5f});
        QVERIFY(w.r > w.g && w.g > w.b);
        const fxmath::BlackWhiteParams bw = fxmath::blackWhiteParams(
            EffectRegistry::instance().defaultParams(fxid::BlackWhite), 0);
        const fxmath::Rgb grey = fxmath::blackWhite(bw, c);
        QCOMPARE(grey.r, grey.b);
        // Colour balance defaults are the identity; gain doubles.
        const ParamSet cbDefaults = EffectRegistry::instance().defaultParams(fxid::ColorBalance);
        const fxmath::Rgb same = fxmath::colorBalance(fxmath::colorBalanceParams(cbDefaults, 0), c);
        QVERIFY(std::abs(same.r - c.r) < 1e-5f && std::abs(same.b - c.b) < 1e-5f);
        ParamSet gain = cbDefaults;
        gain.setBase(k::Gain, 200);
        QCOMPARE(fxmath::colorBalance(fxmath::colorBalanceParams(gain, 0), {0.25f, 0.25f, 0.25f}).r,
            0.5f);
    }

    // ---- GPU against the CPU reference (pointwise effects) ----

    void pointEffectsMatchReference_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QVariantMap>("values");
        QTest::addColumn<QVariantMap>("statics");
        auto row = [](const char* name, const char* type, QVariantMap v, QVariantMap s = {}) {
            QTest::newRow(name) << QString::fromLatin1(type) << v << s;
        };
        row("chroma_key", fxid::ChromaKey, {});
        row("chroma_key_blue_max_spill", fxid::ChromaKey,
            {{k::Tolerance, 25.0}, {k::Softness, 40.0}, {k::SpillAmount, 100.0}},
            {{k::Screen, fxchoice::Blue}, {k::SpillMethod, fxchoice::Maximum}});
        row("chroma_key_matte_view", fxid::ChromaKey, {{k::ClipBlack, 10.0}, {k::ClipWhite, 80.0}},
            {{k::View, fxchoice::Matte}});
        row("chroma_key_status_invert", fxid::ChromaKey, {},
            {{k::View, fxchoice::Status}, {k::Invert, true}});
        row("luma_key", fxid::LumaKey, {{k::Low, 30.0}, {k::High, 80.0}});
        row("luma_key_matte", fxid::LumaKey, {}, {{k::View, fxchoice::Matte}});
        row("hsl_key", fxid::HslKey, {{k::Hue, 140.0}, {k::HueWidth, 80.0}});
        row("hsl_key_inverted", fxid::HslKey, {{k::Hue, 20.0}}, {{k::Invert, true}});
        row("exposure", fxid::Exposure, {{k::Exposure, 0.7}, {k::Gamma, 1.3}});
        row("temperature_tint", fxid::TemperatureTint, {{k::Temperature, -40.0}, {k::Tint, 30.0}});
        row("levels", fxid::Levels,
            {{k::InBlack, 20.0}, {k::InWhite, 220.0}, {k::Gamma, 0.8}, {k::OutBlack, 10.0},
                {k::OutWhite, 240.0}});
        row("invert", fxid::Invert, {{k::Amount, 70.0}});
        row("black_white_tint", fxid::BlackWhite, {{k::Amount, 90.0}, {k::TintAmount, 60.0}});
        row("posterize", fxid::Posterize, {{k::Levels, 4.0}});
        row("color_balance", fxid::ColorBalance,
            {{k::Lift, 10.0}, {k::LiftB, 20.0}, {k::Gamma, -30.0}, {k::GammaR, 15.0},
                {k::Gain, 120.0}, {k::GainG, -10.0}});
    }
    void pointEffectsMatchReference()
    {
        QFETCH(QString, type);
        QFETCH(QVariantMap, values);
        QFETCH(QVariantMap, statics);
        const QColor background(20, 30, 200);
        int worst = 0;
        for (const bool gpu : {true, false}) {
            if (gpu && !gpuReady())
                continue;
            Compositor& c = gpu ? *m_gpu : *m_cpu;
            for (const QColor& colour : kSwatches) {
                TimelineData tl = emptyTimeline(64, 36);
                addClip(tl, TrackType::Video, 0, solidClip(tl, background, 0, 10));
                ClipData fg = solidClip(tl, colour, 0, 10);
                EffectInstance e = EffectRegistry::instance().createEffect(type, tl.allocateId());
                for (auto it = values.cbegin(); it != values.cend(); ++it)
                    e.params.setBase(it.key(), it.value().toDouble());
                for (auto it = statics.cbegin(); it != statics.cend(); ++it)
                    e.params.setStatic(it.key(), it.value());
                fg.effects.append(e);
                // Expected: the CPU reference on the 8-bit colour, "over" the background.
                QImage expected(1, 1, QImage::Format_ARGB32_Premultiplied);
                expected.fill(colour);
                QVERIFY(fxmath::applyPointEffect(type, e.params, 0, expected));
                addClip(tl, TrackType::Video, 1, std::move(fg));
                const Px out = px(render(c, tl), 32, 18);
                // The raw (premultiplied) value, not QImage::pixel()'s conversion.
                const QRgb fgPx = reinterpret_cast<const QRgb*>(expected.constScanLine(0))[0];
                const double a = qAlpha(fgPx) / 255.0;
                // Premultiplied over: fg + bg · (1 − a).
                const int er = int(std::lround(qRed(fgPx) + background.red() * (1 - a)));
                const int eg = int(std::lround(qGreen(fgPx) + background.green() * (1 - a)));
                const int eb = int(std::lround(qBlue(fgPx) + background.blue() * (1 - a)));
                const int diff
                    = std::max({std::abs(out.r - er), std::abs(out.g - eg), std::abs(out.b - eb)});
                worst = std::max(worst, diff);
                QVERIFY2(diff <= 3,
                    qPrintable(QStringLiteral("%1 %2 on %3: got %4, expected rgb(%5,%6,%7)")
                                   .arg(gpu ? QStringLiteral("GPU") : QStringLiteral("CPU"), type,
                                       colour.name(), QString::fromLatin1(describe(out)))
                                   .arg(er)
                                   .arg(eg)
                                   .arg(eb)));
            }
        }
        qInfo("%s: largest difference to the reference %d", qPrintable(type), worst);
    }

    // ---- Chroma key on a green-screen frame ----

    void chromaKeyGreenScreen()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        if (!m_screenError.isEmpty())
            QSKIP(qPrintable(m_screenError));
        for (const QString& file : {m_screen8, m_screen10}) {
            m_gpu->setMediaPaths(media(file));
            const TimelineData tl = greenScreenTimeline([](TimelineData& t, ClipData& fg) {
                fg.effects.append(effect(t, fxid::ChromaKey));
            });
            const QImage out = render(*m_gpu, tl);
            // Screen: the red background shows through; skin stays (spill removal does not
            // touch it); the greenish box is partly transparent.
            QVERIFY2(near(px(out, 150, 80), 220, 0, 0, 255, 3), describe(px(out, 150, 80)));
            QVERIFY2(near(px(out, 2, 2), 220, 0, 0, 255, 3), describe(px(out, 2, 2)));
            QVERIFY2(near(px(out, 80, 45), 230, 166, 128, 255, 4), describe(px(out, 80, 45)));
            const Px partial = px(out, 17, 17);
            QVERIFY2(partial.r > 30 && partial.r < 215 && partial.g > 20, describe(partial));
        }
    }

    void chromaKeyStableAcrossBitDepth()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        if (!m_screenError.isEmpty())
            QSKIP(qPrintable(m_screenError));
        QImage mattes[2];
        for (int i = 0; i < 2; ++i) {
            m_gpu->setMediaPaths(media(i == 0 ? m_screen8 : m_screen10));
            const TimelineData tl = greenScreenTimeline([](TimelineData& t, ClipData& fg) {
                fg.effects.append(effect(
                    t, fxid::ChromaKey, {{k::Softness, 40.0}}, {{k::View, fxchoice::Matte}}));
            });
            mattes[i] = render(*m_gpu, tl);
        }
        int maxDiff = 0;
        double sum = 0;
        for (int y = 0; y < 90; ++y) {
            for (int x = 0; x < 160; ++x) {
                const int d = std::abs(qGray(mattes[0].pixel(x, y)) - qGray(mattes[1].pixel(x, y)));
                maxDiff = std::max(maxDiff, d);
                sum += d;
            }
        }
        qInfo("8 vs 10 bit matte: max difference %d, mean %.3f", maxDiff, sum / (160 * 90));
        QVERIFY(maxDiff <= 8);
        QVERIFY(sum / (160 * 90) < 1.0);
        // Matte view: opaque greyscale, white = kept, black = keyed.
        QVERIFY(near(px(mattes[0], 80, 45), 255, 255, 255, 255, 1));
        QVERIFY(near(px(mattes[0], 150, 80), 0, 0, 0, 255, 1));
    }

    void chromaKeyMatteProcessing()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        if (!m_screenError.isEmpty())
            QSKIP(qPrintable(m_screenError));
        m_gpu->setMediaPaths(media(m_screen8));
        auto matte = [&](double shrinkGrow, double edgeBlur) {
            const TimelineData tl = greenScreenTimeline([&](TimelineData& t, ClipData& fg) {
                fg.effects.append(effect(t, fxid::ChromaKey,
                    {{k::ShrinkGrow, shrinkGrow}, {k::EdgeBlur, edgeBlur}},
                    {{k::View, fxchoice::Matte}}));
            });
            return render(*m_gpu, tl);
        };
        const QImage plain = matte(0, 0);
        // The skin box spans x 40..119: 3 px outside / inside its left edge.
        QCOMPARE(qGray(plain.pixel(37, 45)), 0);
        QCOMPARE(qGray(plain.pixel(42, 45)), 255);
        const QImage grown = matte(5, 0);
        QCOMPARE(qGray(grown.pixel(37, 45)), 255);
        const QImage shrunk = matte(-5, 0);
        QCOMPARE(qGray(shrunk.pixel(42, 45)), 0);
        QCOMPARE(qGray(shrunk.pixel(80, 45)), 255);
        const QImage blurred = matte(0, 6);
        const int edge = qGray(blurred.pixel(40, 45));
        QVERIFY2(edge > 40 && edge < 215, QByteArray::number(edge));
        QCOMPARE(qGray(blurred.pixel(80, 45)), 255);
    }

    void garbageMatteRestrictsTheKey()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        if (!m_screenError.isEmpty())
            QSKIP(qPrintable(m_screenError));
        m_gpu->setMediaPaths(media(m_screen8));
        // A rectangle mask over the left half: the key keeps the skin inside it; outside the
        // mask the clip is gone entirely (also the skin box's right part).
        const TimelineData tl = greenScreenTimeline([](TimelineData& t, ClipData& fg) {
            fg.effects.append(effect(t, fxid::ChromaKey));
            MaskData m = makeMask(MaskShape::Rectangle, t.allocateId(), QSizeF(80, 90));
            m.params.setBase(maskkeys::PosX, -40);
            fg.masks.append(m);
        });
        const QImage out = render(*m_gpu, tl);
        QVERIFY2(near(px(out, 60, 45), 230, 166, 128, 255, 4), describe(px(out, 60, 45)));
        QVERIFY2(near(px(out, 100, 45), 220, 0, 0, 255, 3), describe(px(out, 100, 45)));
        QVERIFY2(near(px(out, 20, 80), 220, 0, 0, 255, 3), describe(px(out, 20, 80)));
    }

    void chromaKeyOnCpuFallback()
    {
        if (!m_screenError.isEmpty())
            QSKIP(qPrintable(m_screenError));
        m_cpu->setMediaPaths(media(m_screen8));
        const TimelineData tl = greenScreenTimeline(
            [](TimelineData& t, ClipData& fg) { fg.effects.append(effect(t, fxid::ChromaKey)); });
        const QImage out = render(*m_cpu, tl);
        QVERIFY2(near(px(out, 150, 80), 220, 0, 0, 255, 4), describe(px(out, 150, 80)));
        QVERIFY2(near(px(out, 80, 45), 230, 166, 128, 255, 6), describe(px(out, 80, 45)));
    }

    void chromaKeyPerformance()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        // 1080p solid clip keyed over a background: the keyer alone and with shrink / grow +
        // edge blur. Times include readback; compared against the same timeline without it.
        TimelineData base = emptyTimeline(1920, 1080);
        addClip(base, TrackType::Video, 0, solidClip(base, Qt::red, 0, 100));
        ClipData fg = solidClip(base, QColor(0, 177, 64), 0, 100);
        fg.masks.append(
            makeMask(MaskShape::Ellipse, base.allocateId(), QSizeF(1600, 900))); // layer path
        TimelineData plain = base;
        addClip(plain, TrackType::Video, 1, fg);
        auto timeFrames = [&](const TimelineData& tl) {
            const auto s = snap(tl);
            (void)m_gpu->renderImage(s, 0); // warm-up (shader, targets)
            QElapsedTimer t;
            t.start();
            constexpr int kFrames = 20;
            for (int i = 0; i < kFrames; ++i)
                (void)m_gpu->renderImage(s, i);
            return double(t.nsecsElapsed()) / 1e6 / kFrames;
        };
        TimelineData keyed = base;
        ClipData keyedFg = fg;
        keyedFg.effects.append(effect(keyed, fxid::ChromaKey));
        addClip(keyed, TrackType::Video, 1, keyedFg);
        TimelineData processed = base;
        ClipData processedFg = fg;
        processedFg.effects.append(
            effect(processed, fxid::ChromaKey, {{k::ShrinkGrow, -3.0}, {k::EdgeBlur, 4.0}}));
        addClip(processed, TrackType::Video, 1, processedFg);
        const double plainMs = timeFrames(plain);
        const double keyMs = timeFrames(keyed);
        const double processedMs = timeFrames(processed);
        qInfo("1080p frame incl. readback (%s): without key %.2f ms, chroma key %.2f ms, with "
              "shrink 3 px + edge blur 4 px %.2f ms",
            qPrintable(m_gpu->backendDescription()), plainMs, keyMs, processedMs);
    }

    // ---- Spatial effects (analytic expectations) ----

    void blursSmooth()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        QString why;
        if (!ensureFixtures(&why))
            QSKIP(qPrintable(why));
        TimelineData tl = emptyTimeline(640, 360);
        addClip(tl, TrackType::Video, 0,
            mediaClip(tl, QStringLiteral("v"), 0, 100, 2'000'000, 10'000'000));
        auto map = std::make_shared<MediaPathMap>();
        map->insert(QStringLiteral("v"), fixture("h264_25fps.mp4"));
        m_gpu->setMediaPaths(map);
        const QImage plain = render(*m_gpu, tl);
        const double sharp = highFrequencyEnergy(plain);
        auto with = [&](EffectInstance e) {
            TimelineData t = tl;
            t.videoTracks[0].clips[0].effects.append(e);
            return render(*m_gpu, t);
        };
        for (const auto& [name, e] :
            {std::pair {"box", effect(tl, fxid::BoxBlur, {{k::Radius, 6.0}})},
                std::pair {"box_large", effect(tl, fxid::BoxBlur, {{k::Radius, 80.0}})},
                std::pair {"directional", effect(tl, fxid::DirectionalBlur, {{k::Length, 30.0}})},
                std::pair {
                    "directional_long", effect(tl, fxid::DirectionalBlur, {{k::Length, 300.0}})},
                std::pair {"radial", effect(tl, fxid::RadialBlur, {{k::Angle, 20.0}})},
                std::pair {"zoom", effect(tl, fxid::ZoomBlur, {{k::Amount, 30.0}})}}) {
            const QImage img = with(e);
            const double energy = highFrequencyEnergy(img);
            qInfo("%s: HF energy %.2f of the original", name, energy / sharp);
            QVERIFY2(energy < 0.6 * sharp, name);
        }
        // Directional blur at 0 degrees spreads a letter sideways, not up or down.
        {
            TimelineData t = emptyTimeline(320, 180);
            addClip(t, TrackType::Video, 0, titleClip(t, QStringLiteral("I"), 0, 10, 120));
            const QImage letter = render(*m_gpu, t);
            t.videoTracks[0].clips[0].effects.append(
                effect(t, fxid::DirectionalBlur, {{k::Length, 40.0}}));
            const QImage smeared = render(*m_gpu, t);
            auto extent = [](const QImage& img, bool horizontal) {
                int n = 0;
                for (int i = 0; i < (horizontal ? img.width() : img.height()); ++i)
                    n += qGray(horizontal ? img.pixel(i, 90) : img.pixel(160, i)) > 3 ? 1 : 0;
                return n;
            };
            qInfo("directional: letter %d x %d px, blurred %d x %d px", extent(letter, true),
                extent(letter, false), extent(smeared, true), extent(smeared, false));
            QVERIFY(extent(smeared, true) >= extent(letter, true) + 30);
            QVERIFY(std::abs(extent(smeared, false) - extent(letter, false)) <= 2);
        }
        // Zero amounts are skipped (identical picture).
        QCOMPARE(with(effect(tl, fxid::BoxBlur, {{k::Radius, 0.0}})), plain);
        QCOMPARE(with(effect(tl, fxid::ZoomBlur, {{k::Amount, 0.0}})), plain);
    }

    void lightAndStylize()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(320, 180);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 10));
        addClip(tl, TrackType::Video, 1, titleClip(tl, QStringLiteral("I"), 0, 10, 120));
        auto with = [&](EffectInstance e) {
            TimelineData t = tl;
            t.videoTracks[1].clips[0].effects.append(e);
            return render(*m_gpu, t);
        };
        const QImage plain = render(*m_gpu, tl);
        // Find the letter's right edge on the middle row.
        int edge = -1;
        for (int x = 319; x > 0 && edge < 0; --x) {
            if (qGray(plain.pixel(x, 90)) > 128)
                edge = x;
        }
        QVERIFY(edge > 160);
        const QImage glow = with(effect(tl, fxid::Glow, {{k::Threshold, 50.0}, {k::Radius, 20.0}}));
        QVERIFY(qGray(plain.pixel(edge + 8, 90)) < 5);
        QVERIFY2(qGray(glow.pixel(edge + 8, 90)) > 15,
            QByteArray::number(qGray(glow.pixel(edge + 8, 90))));
        const QImage ca = with(effect(tl, fxid::ChromaticAberration, {{k::Amount, 8.0}}));
        bool fringe = false;
        for (int x = edge - 6; x <= edge + 6; ++x) {
            const QRgb p = ca.pixel(x, 90);
            fringe = fringe || std::abs(qRed(p) - qBlue(p)) > 60;
        }
        QVERIFY(fringe);
        const QImage edges = with(effect(tl, fxid::EdgeDetect));
        QVERIFY(qGray(edges.pixel(edge, 90)) > 100 || qGray(edges.pixel(edge + 1, 90)) > 100);
        const QImage stroke
            = with(effect(tl, fxid::Stroke, {{k::Width, 4.0}}, {{k::Color, QColor(Qt::red)}}));
        QVERIFY2(qRed(stroke.pixel(edge + 2, 90)) > 200 && qGreen(stroke.pixel(edge + 2, 90)) < 60,
            describe(px(stroke, edge + 2, 90)));
        QVERIFY(qGray(stroke.pixel(edge + 10, 90)) < 5);
        const QImage inside = with(effect(tl, fxid::Stroke, {{k::Width, 3.0}},
            {{k::Color, QColor(Qt::red)}, {k::Position, fxchoice::Inside}}));
        QVERIFY2(qRed(inside.pixel(edge - 1, 90)) > 200 && qGreen(inside.pixel(edge - 1, 90)) < 60,
            describe(px(inside, edge - 1, 90)));
        QVERIFY(qGray(inside.pixel(edge + 3, 90)) < 5);

        // Solid-colour expectations: emboss of a flat picture is mid grey, edge detect black.
        TimelineData flat = emptyTimeline(64, 36);
        addClip(flat, TrackType::Video, 0, solidClip(flat, QColor(200, 100, 50), 0, 10));
        auto flatWith = [&](EffectInstance e) {
            TimelineData t = flat;
            t.videoTracks[0].clips[0].effects.append(e);
            return px(render(*m_gpu, t), 32, 18);
        };
        QVERIFY(near(flatWith(effect(flat, fxid::Emboss)), 128, 128, 128, 255, 2));
        QVERIFY(near(flatWith(effect(flat, fxid::EdgeDetect)), 0, 0, 0, 255, 2));
    }

    void filmGrain()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(160, 90);
        ClipData grey = solidClip(tl, QColor(128, 128, 128), 0, 10);
        grey.effects.append(effect(tl, fxid::FilmGrain, {{k::Amount, 50.0}}));
        addClip(tl, TrackType::Video, 0, grey);
        auto stats = [](const QImage& img, double* mean, double* sd) {
            double s = 0, s2 = 0;
            const int n = img.width() * img.height();
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    const double g = qGray(img.pixel(x, y));
                    s += g;
                    s2 += g * g;
                }
            }
            *mean = s / n;
            *sd = std::sqrt(std::max(0.0, s2 / n - *mean * *mean));
        };
        const QImage f0 = render(*m_gpu, tl, 0);
        double mean = 0, sd = 0;
        stats(f0, &mean, &sd);
        qInfo("grain: mean %.1f, sd %.1f", mean, sd);
        QVERIFY(std::abs(mean - 128) < 4);
        QVERIFY(sd > 5);
        QVERIFY(f0 != render(*m_gpu, tl, 1)); // animated
        tl.videoTracks[0].clips[0].effects[0].params.setStatic(k::Animated, false);
        QCOMPARE(render(*m_gpu, tl, 0), render(*m_gpu, tl, 1));
        // The GPU grain uses the CPU's hash: values agree within the 8-bit rounding.
        int agree = 0;
        const QImage still = render(*m_gpu, tl, 0);
        for (int x = 0; x < 160; ++x) {
            const float n = fxmath::grainNoise(float(x), 40.f, 1.5f, 0);
            const int expected = to8(128 / 255.f + n * 0.5f * 0.4f);
            agree += std::abs(qGray(still.pixel(x, 40)) - expected) <= 2 ? 1 : 0;
        }
        QVERIFY2(agree >= 150, QByteArray::number(agree));
    }

    void distortAndCrop()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(160, 90);
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(0, 0, 220), 0, 10));
        addClip(tl, TrackType::Video, 1, solidClip(tl, QColor(240, 240, 0), 0, 10));
        auto with = [&](EffectInstance e) {
            TimelineData t = tl;
            t.videoTracks[1].clips[0].effects.append(e);
            return render(*m_gpu, t);
        };
        // Barrel distortion pulls the corners in: the background shows there, the centre
        // keeps the colour.
        const QImage barrel = with(effect(tl, fxid::LensDistortion, {{k::Distortion, 100.0}}));
        QVERIFY2(near(px(barrel, 1, 1), 0, 0, 220, 255, 3), describe(px(barrel, 1, 1)));
        QVERIFY(near(px(barrel, 80, 45), 240, 240, 0, 255, 2));
        // Pincushion with zoom 100 % keeps the frame covered.
        const QImage pin = with(effect(tl, fxid::LensDistortion, {{k::Distortion, -60.0}}));
        QVERIFY(near(px(pin, 1, 1), 240, 240, 0, 255, 3));
        // Waves of a uniform picture are uniform.
        QVERIFY(near(px(with(effect(tl, fxid::Ripple, {{k::Amplitude, 20.0}})), 50, 30), 240, 240,
            0, 255, 2));
        QVERIFY(near(
            px(with(effect(tl, fxid::Wave, {{k::Amplitude, 20.0}})), 50, 30), 240, 240, 0, 255, 2));
        // Feathered crop: 25 % off the left with a 10 px feather inwards from the crop line.
        const QImage crop
            = with(effect(tl, fxid::FeatheredCrop, {{k::Left, 25.0}, {k::Feather, 10.0}}));
        QVERIFY(near(px(crop, 20, 45), 0, 0, 220, 255, 2)); // cropped
        QVERIFY(near(px(crop, 80, 45), 240, 240, 0, 255, 2)); // kept
        const int mid = qRed(crop.pixel(45, 45));
        QVERIFY2(mid > 30 && mid < 210, QByteArray::number(mid)); // inside the feather
        QVERIFY(qRed(crop.pixel(42, 45)) < qRed(crop.pixel(47, 45)));

        // Ripple / wave move pixels of a textured picture (the title letter).
        TimelineData t2 = emptyTimeline(320, 180);
        addClip(t2, TrackType::Video, 0, titleClip(t2, QStringLiteral("I"), 0, 10, 120));
        const QImage letter = render(*m_gpu, t2);
        t2.videoTracks[0].clips[0].effects.append(effect(t2, fxid::Wave, {{k::Amplitude, 15.0}}));
        QVERIFY(render(*m_gpu, t2) != letter);
    }

    // ---- End to end: an exported chroma key ----

    void chromaKeyExportEndToEnd()
    {
        if (!m_screenError.isEmpty())
            QSKIP(qPrintable(m_screenError));
        const TimelineData tl = greenScreenTimeline(
            [](TimelineData& t, ClipData& fg) { fg.effects.append(effect(t, fxid::ChromaKey)); });
        ExportSettings s;
        s.range = RangeMode::Custom;
        s.rangeIn = 0;
        s.rangeOut = 5;
        s.audio.enabled = false;
        s.outputPath = m_dir.filePath(QStringLiteral("keyed.mp4"));
        ExportJob job(s, snap(tl), media(m_screen8));
        job.setCompositor(m_gpu.get());
        const ExportResult r = job.run({});
        QVERIFY2(r.ok, qPrintable(r.error));
        media::VideoDecoder decoder;
        QVERIFY(decoder.open(r.outputPath));
        const QImage frame = decoder.frameAt(80'000).convertToFormat(QImage::Format_RGB32);
        QCOMPARE(frame.size(), QSize(160, 90));
        // Lossy H.264 with 4:2:0: generous tolerances, but the colours must be the right ones.
        QVERIFY2(near(px(frame, 145, 75), 220, 0, 0, 255, 20), describe(px(frame, 145, 75)));
        QVERIFY2(near(px(frame, 80, 45), 230, 166, 128, 255, 20), describe(px(frame, 80, 45)));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestVideoEffects>(argc, argv);
}

#include "test_videoeffects.moc"
