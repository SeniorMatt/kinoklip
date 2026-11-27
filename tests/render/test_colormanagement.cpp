// SPDX-License-Identifier: GPL-3.0-or-later
// The colour management effects (docs/behavior-spec.md section 205): Color Space Transform, ACES
// Transform, Gamut Mapping, Chromatic Adaptation, Color Compressor, DCTL and the DCTL
// Transition. Registry, the CPU path against the maths of core/ColorSpaces.h, the GPU against
// the CPU, the DCTL translator (supported files, clear errors) and DCTL files drawn on the GPU.
// GPU cases skip without OpenGL.
#include "RenderTestUtil.h"
#include "core/DctlTranslator.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "render/ColorManagementFx.h"
#include "render/Compositor.h"
#include "render/GlEffects.h"
#include "render/GlTransitions.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <set>

using namespace kk;
using namespace kk::test;
using namespace kk::render;

namespace {

namespace k = fxkeys;
namespace c = fxchoice;

const char* const kEffects[] = {fxid::ColorSpaceTransform, fxid::AcesTransform, fxid::GamutMapping,
    fxid::ChromaticAdaptation, fxid::ColorCompressor, fxid::Dctl};

QString exampleDir()
{
    return QFileInfo(QStringLiteral(__FILE__)).dir().absoluteFilePath(QStringLiteral("../../resources/dctl"));
}

QString readExample(const char* name)
{
    QFile f(exampleDir() + QLatin1Char('/') + QLatin1String(name));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

const QColor kSwatches[] = {QColor(200, 100, 50), QColor(230, 166, 128), QColor(30, 200, 90),
    QColor(0, 71, 187), QColor(128, 128, 128), QColor(20, 20, 20), QColor(240, 240, 240),
    QColor(90, 160, 220), QColor(250, 40, 160), QColor(10, 250, 120)};

} // namespace

class TestColorManagement : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    bool gpuReady() const { return m_gpu && m_gpu->isGpu(); }

    QImage render(Compositor& comp, const TimelineData& tl, int64_t frame = 0)
    {
        return comp.renderImage(snap(tl), frame);
    }

    /// One pixel of @p colour with @p e on it.
    Px onSwatch(Compositor& comp, const QColor& colour, const EffectInstance& e)
    {
        TimelineData tl = emptyTimeline(64, 36);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 10));
        ClipData fg = solidClip(tl, colour, 0, 10);
        fg.effects.append(e);
        addClip(tl, TrackType::Video, 1, std::move(fg));
        return px(render(comp, tl), 32, 18);
    }

    EffectInstance make(const char* type, const QVariantMap& values = {},
        const QVariantMap& statics = {})
    {
        EffectInstance e = EffectRegistry::instance().createEffect(QLatin1String(type), 77);
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            e.params.setBase(it.key(), it.value().toDouble());
        for (auto it = statics.cbegin(); it != statics.cend(); ++it)
            e.params.setStatic(it.key(), it.value());
        return e;
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
    }
    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    // ---- registry ----

    void effectsAreRegistered()
    {
        std::set<QString> gpuIds;
        for (const GlEffectFactory& f : builtinGlEffects())
            gpuIds.insert(QLatin1String(f.id));
        const QStringList names {QStringLiteral("Color Space Transform"),
            QStringLiteral("ACES Transform"), QStringLiteral("Gamut Mapping"),
            QStringLiteral("Chromatic Adaptation"), QStringLiteral("Color Compressor"),
            QStringLiteral("DCTL")};
        int n = 0;
        for (const char* id : kEffects) {
            const EffectDescriptor* d = EffectRegistry::instance().find(QLatin1String(id));
            QVERIFY2(d, id);
            QCOMPARE(d->category, EffectCategory::VideoEffect);
            QCOMPARE(d->group, QLatin1String(effectgroups::Color));
            QCOMPARE(d->name, names[n++]);
            QVERIFY(!d->description.isEmpty());
            QVERIFY2(gpuIds.count(d->id), id);
            const ParamSet defaults = EffectRegistry::instance().defaultParams(d->id);
            for (const ParamDescriptor& p : d->params) {
                if (p.isScalar())
                    QVERIFY2(p.def >= p.min && p.def <= p.max, qPrintable(d->id + p.key));
                else if (p.type == ParamType::Choice)
                    QVERIFY2(p.choices.contains(p.defStatic.toString()), qPrintable(d->id + p.key));
            }
        }
        // The Resolve names of the Color Space Transform parameters.
        const EffectDescriptor* cst
            = EffectRegistry::instance().find(QLatin1String(fxid::ColorSpaceTransform));
        QStringList labels;
        for (const ParamDescriptor& p : cst->params)
            labels << p.label;
        for (const QString& want : {QStringLiteral("Input Color Space"),
                 QStringLiteral("Input Gamma"), QStringLiteral("Tone Mapping"),
                 QStringLiteral("Output Color Space"), QStringLiteral("Output Gamma"),
                 QStringLiteral("Gamut Mapping"), QStringLiteral("Apply Forward OOTF"),
                 QStringLiteral("Apply Inverse OOTF"), QStringLiteral("Use White Point Adaptation")})
            QVERIFY2(labels.contains(want), qPrintable(want));
        // The transition sits in the Resolve FX Transitions folder with a GPU implementation.
        const EffectDescriptor* t = EffectRegistry::instance().find(QLatin1String(fxid::DctlTransition));
        QVERIFY(t);
        QCOMPARE(t->category, EffectCategory::Transition);
        QCOMPARE(t->name, QStringLiteral("DCTL Transition"));
        QVERIFY(findGlTransitionFactory(QLatin1String(fxid::DctlTransition)));
    }

    // ---- the pointwise effects: CPU path and GPU against the maths ----

    void effectsMatchTheMaths_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QVariantMap>("values");
        QTest::addColumn<QVariantMap>("statics");
        auto row = [](const char* name, const char* type, QVariantMap v = {}, QVariantMap s = {}) {
            QTest::newRow(name) << QString::fromLatin1(type) << v << s;
        };
        row("cst default (DaVinci to 709)", fxid::ColorSpaceTransform);
        row("cst 709 to ACEScct", fxid::ColorSpaceTransform, {},
            {{k::CmInGamut, QStringLiteral("rec709")}, {k::CmInGamma, QStringLiteral("gamma24")}, {k::CmOutGamut, QStringLiteral("acesap1")},
                {k::CmOutGamma, QStringLiteral("acescct")}});
        row("cst sRGB to P3 gamma 2.6", fxid::ColorSpaceTransform, {},
            {{k::CmInGamut, QStringLiteral("rec709")}, {k::CmInGamma, QStringLiteral("srgb")}, {k::CmOutGamut, QStringLiteral("p3d65")},
                {k::CmOutGamma, QStringLiteral("gamma26")}});
        row("cst 2020 to 709 with compression", fxid::ColorSpaceTransform, {},
            {{k::CmInGamut, QStringLiteral("rec2020")}, {k::CmInGamma, QStringLiteral("gamma24")}, {k::CmOutGamut, QStringLiteral("rec709")},
                {k::CmOutGamma, QStringLiteral("gamma24")}, {k::CmGamutMap, c::GmCompression}});
        row("cst 2020 to 709 with saturation mapping", fxid::ColorSpaceTransform, {},
            {{k::CmInGamut, QStringLiteral("rec2020")}, {k::CmInGamma, QStringLiteral("gamma24")}, {k::CmOutGamut, QStringLiteral("rec709")},
                {k::CmOutGamma, QStringLiteral("gamma24")}, {k::CmGamutMap, c::GmSaturation}});
        row("cst PQ to 709 tone mapped", fxid::ColorSpaceTransform, {},
            {{k::CmInGamut, QStringLiteral("rec2020")}, {k::CmInGamma, QStringLiteral("pq")}, {k::CmOutGamut, QStringLiteral("rec709")},
                {k::CmOutGamma, QStringLiteral("gamma24")}, {k::CmTone, c::ToneLuminance}});
        row("cst HLG to 709 with OOTF", fxid::ColorSpaceTransform, {},
            {{k::CmInGamut, QStringLiteral("rec2020")}, {k::CmInGamma, QStringLiteral("hlg")}, {k::CmOutGamut, QStringLiteral("rec709")},
                {k::CmOutGamma, QStringLiteral("gamma24")}, {k::CmFwdOotf, true}, {k::CmTone, c::ToneDavinci}});
        row("cst LogC to ACEScg no adaptation", fxid::ColorSpaceTransform, {},
            {{k::CmInGamut, QStringLiteral("alexawide")}, {k::CmInGamma, QStringLiteral("logc3")}, {k::CmOutGamut, QStringLiteral("acesap1")},
                {k::CmOutGamma, QStringLiteral("linear")}, {k::CmAdapt, false}});
        row("aces sRGB to ACEScg", fxid::AcesTransform);
        row("aces ACEScg to sRGB", fxid::AcesTransform, {},
            {{k::AcesInput, QStringLiteral("acescg")}, {k::AcesOutput, QStringLiteral("srgb")}});
        row("aces ACEScct to Rec.709", fxid::AcesTransform, {},
            {{k::AcesInput, QStringLiteral("acescct")}, {k::AcesOutput, QStringLiteral("rec709")}});
        row("aces inverse output transform", fxid::AcesTransform, {},
            {{k::AcesInput, QStringLiteral("srgb")}, {k::AcesOutput, QStringLiteral("acescg")}, {k::AcesInvert, true}});
        row("gamut mapping", fxid::GamutMapping);
        row("gamut mapping saturation", fxid::GamutMapping, {{k::Amount, 60.0}},
            {{k::CmGmMode, c::GmSaturation}});
        row("chromatic adaptation", fxid::ChromaticAdaptation);
        row("chromatic adaptation cat02", fxid::ChromaticAdaptation, {{k::Amount, 50.0}},
            {{k::CmMethod, c::CatCat02}, {k::CmDstWhite, QStringLiteral("a")}});
        row("color compressor", fxid::ColorCompressor, {{k::CmHiThreshold, 50.0}, {k::CmHiLimit, 150.0}});
    }
    void effectsMatchTheMaths()
    {
        QFETCH(QString, type);
        QFETCH(QVariantMap, values);
        QFETCH(QVariantMap, statics);
        const QByteArray t = type.toLatin1();
        const EffectInstance e = make(t.constData(), values, statics);
        const cm::CstParams cp = fxmath::colorManagementParams(type, e.params, 0);
        int worst = 0;
        auto check = [&](Compositor& comp, const char* name) {
            for (const QColor& colour : kSwatches) {
                const float in[3] = {float(colour.redF()), float(colour.greenF()),
                    float(colour.blueF())};
                float want[3];
                cm::cstApply(cp, in, want);
                const Px out = onSwatch(comp, colour, e);
                for (int ch = 0; ch < 3; ++ch) {
                    const int w = int(std::lround(std::clamp(want[ch], 0.f, 1.f) * 255.f));
                    const int got = ch == 0 ? out.r : (ch == 1 ? out.g : out.b);
                    worst = std::max(worst, std::abs(got - w));
                    QVERIFY2(std::abs(got - w) <= 3,
                        qPrintable(QStringLiteral("%1 %2 on %3: channel %4 got %5 want %6")
                                       .arg(QLatin1String(name), type, colour.name())
                                       .arg(ch)
                                       .arg(got)
                                       .arg(w)));
                }
            }
        };
        check(*m_cpu, "CPU");
        if (gpuReady())
            check(*m_gpu, "GPU");
    }

    void defaultTransformChangesThePicture()
    {
        // DaVinci Wide Gamut / Intermediate into Rec.709: grey 18 % in, grey 18 % out.
        const float di = cm::encode(cm::Transfer::DavinciIntermediate, 0.18f);
        const QColor in = QColor::fromRgbF(di, di, di);
        const Px out = onSwatch(*m_cpu, in, make(fxid::ColorSpaceTransform));
        const int want = int(std::lround(std::pow(0.18, 1.0 / 2.4) * 255.0));
        QVERIFY(std::abs(out.g - want) <= 2);
        QVERIFY(std::abs(out.r - out.b) <= 2);
        // A neutral transform does nothing.
        const EffectInstance same = make(fxid::ColorSpaceTransform, {},
            {{k::CmInGamut, QStringLiteral("rec709")}, {k::CmInGamma, QStringLiteral("gamma24")}});
        const Px p = onSwatch(*m_cpu, QColor(90, 160, 220), same);
        QVERIFY(std::abs(p.r - 90) <= 1 && std::abs(p.g - 160) <= 1 && std::abs(p.b - 220) <= 1);
    }

    // ---- DCTL translator ----

    void examplesTranslate_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("entry");
        QTest::addColumn<int>("params");
        QTest::newRow("exposure") << "exposure_contrast.dctl" << int(dctl::Entry::Transform) << 4;
        QTest::newRow("mono tint") << "mono_tint.dctl" << int(dctl::Entry::Transform) << 4;
        QTest::newRow("push") << "push_slide.dctl" << int(dctl::Entry::Transition) << 1;
    }
    void examplesTranslate()
    {
        QFETCH(QString, file);
        QFETCH(int, entry);
        QFETCH(int, params);
        const QString src = readExample(file.toLatin1().constData());
        QVERIFY2(!src.isEmpty(), qPrintable(exampleDir()));
        const dctl::Result r = dctl::translate(src, dctl::Entry(entry));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.params.size(), params);
        QVERIFY(r.glsl.contains(QLatin1String("void main()")));
        // The wrong entry point is a clear error.
        const dctl::Result other = dctl::translate(
            src, dctl::Entry(entry == int(dctl::Entry::Transform) ? 1 : 0));
        QVERIFY(!other.ok);
        QVERIFY2(other.error.contains(QLatin1String("no transform()"))
                || other.error.contains(QLatin1String("no transition()")),
            qPrintable(other.error));
    }

    void uiParamsAreRead()
    {
        const dctl::Result r = dctl::translate(readExample("mono_tint.dctl"), dctl::Entry::Transform);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.params.size(), 4);
        QCOMPARE(r.params[0].name, QStringLiteral("weights"));
        QCOMPARE(int(r.params[0].kind), int(dctl::UiKind::ComboBox));
        QCOMPARE(r.params[0].options.size(), 3);
        QCOMPARE(int(r.params[2].kind), int(dctl::UiKind::SliderInt));
        QCOMPARE(r.params[2].max, 16.0);
        QCOMPARE(int(r.params[3].kind), int(dctl::UiKind::ColorPicker));
        QCOMPARE(r.params[3].slot, 0);
        QCOMPARE(r.params[3].color[1], 0.85f);
        QCOMPARE(r.params[1].slot, 1); // numeric slots skip the colour picker
    }

    void unsupportedFeaturesAreNamed_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("expect");
        const QString head = QStringLiteral(
            "__DEVICE__ float3 transform(int p_Width, int p_Height, int p_X, int p_Y, float p_R, "
            "float p_G, float p_B)\n{\n");
        QTest::newRow("include") << "#include \"x.h\"\n" + head + "return make_float3(p_R,p_G,p_B);}"
                                 << "#include";
        QTest::newRow("pointer") << head + "float v = 1.0f; float* q = &v;\n return make_float3(p_R,p_G,p_B);}"
                                 << "pointer";
        QTest::newRow("2d array") << "__CONSTANT__ float m[3][3] = {{1,0,0},{0,1,0},{0,0,1}};\n" + head
                + "return make_float3(p_R,p_G,p_B);}"
                                  << "multi-dimensional";
        QTest::newRow("printf") << head + "printf(\"x\");\nreturn make_float3(p_R,p_G,p_B);}"
                                << "printf";
        QTest::newRow("long") << head + "long a = 1;\nreturn make_float3(p_R,p_G,p_B);}" << "long";
        QTest::newRow("unknown param type")
            << "DEFINE_UI_PARAMS(a, A, DCTLUI_BANANA, 1)\n" + head + "return make_float3(p_R,p_G,p_B);}"
            << "DCTLUI_BANANA";
        QTest::newRow("no transform") << "float x;" << "no transform()";
        QTest::newRow("unknown entry parameter")
            << "__DEVICE__ float3 transform(int p_Foo) { return make_float3(0.0f); }" << "p_Foo";
        QTest::newRow("too many params") << [] {
            QString s;
            for (int i = 0; i < 9; ++i)
                s += QStringLiteral("DEFINE_UI_PARAMS(a%1, A, DCTLUI_SLIDER_FLOAT, 0, 0, 1, 0.1)\n").arg(i);
            return s + QStringLiteral("__DEVICE__ float3 transform(int p_Width, int p_Height, int p_X, int p_Y, float p_R, float p_G, float p_B) { return make_float3(p_R,p_G,p_B); }");
        }() << "more than";
    }
    void unsupportedFeaturesAreNamed()
    {
        QFETCH(QString, source);
        QFETCH(QString, expect);
        const dctl::Result r = dctl::translate(source, dctl::Entry::Transform);
        QVERIFY(!r.ok);
        QVERIFY2(r.error.startsWith(QLatin1String("line ")), qPrintable(r.error));
        QVERIFY2(r.error.contains(expect), qPrintable(r.error));
    }

    void cConstructsBecomeGlsl()
    {
        const QString src = QStringLiteral(R"(
typedef struct { float3 a; float b; } Pair;
__CONSTANT__ float kTable[3] = {0.5f, 1.5f, 2.5f};
#define SCALE 2.0f
__DEVICE__ float3 transform(int p_Width, int p_Height, int p_X, int p_Y, float p_R, float p_G, float p_B)
{
    float3 out = make_float3(p_R, p_G, p_B);
    int i = (int)(p_R * 2.0f);
    Pair pr = {{1.0f, 0.5f, 0.25f}, 3.0f};
    unsigned int u = 3;
    out.x = _powf(out.x, 1.0f / 2.2f) * kTable[i] * SCALE * pr.b;
    return out;
}
)");
        const dctl::Result r = dctl::translate(src, dctl::Entry::Transform);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY2(r.glsl.contains(QLatin1String("vec3 dctl_r_out")), qPrintable(r.glsl));
        QVERIFY(r.glsl.contains(QLatin1String("int ( ( p_R * 2.0 ) )")));
        QVERIFY(r.glsl.contains(QLatin1String("struct Pair")) || r.glsl.contains(QLatin1String("struct dctl_t_Pair")));
        QVERIFY(r.glsl.contains(QLatin1String("float[3] ( 0.5")) || r.glsl.contains(QLatin1String("float[3]")));
        QVERIFY(r.glsl.contains(QLatin1String("uint u")));
        QVERIFY(!r.glsl.contains(QLatin1String("0.5f")));
    }

    // ---- DCTL on the GPU ----

    void dctlExposureOnGpu()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QString file = exampleDir() + QStringLiteral("/exposure_contrast.dctl");
        // Defaults of the file: nothing changes. Parameter 1 = +1 stop: twice as bright.
        const QColor in(60, 100, 30);
        const Px same = onSwatch(*m_gpu, in, make(fxid::Dctl, {}, {{k::DctlFile, file}}));
        QVERIFY2(std::abs(same.r - 60) <= 2 && std::abs(same.g - 100) <= 2 && std::abs(same.b - 30) <= 2,
            qPrintable(QStringLiteral("%1 %2 %3").arg(same.r).arg(same.g).arg(same.b)));
        const Px up = onSwatch(
            *m_gpu, in, make(fxid::Dctl, {{k::dctlParam(1), 1.0}}, {{k::DctlFile, file}}));
        QVERIFY2(std::abs(up.r - 120) <= 3 && std::abs(up.g - 200) <= 3 && std::abs(up.b - 60) <= 3,
            qPrintable(QStringLiteral("%1 %2 %3").arg(up.r).arg(up.g).arg(up.b)));
        // The check box flips to "on": the shoulder bends the values above 0.8.
        const Px bright = onSwatch(*m_gpu, QColor(230, 230, 230),
            make(fxid::Dctl, {{k::dctlParam(1), 0.0}, {k::dctlParam(4), 1.0}}, {{k::DctlFile, file}}));
        // 230 / 255 = 0.902: 0.8 + 0.2 * (1 - (1 - 0.1275)^2) = 0.848, 216 of 255
        QVERIFY2(std::abs(bright.r - 216) <= 3, qPrintable(QString::number(bright.r)));
    }

    void dctlMonoTintOnGpu()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QString file = exampleDir() + QStringLiteral("/mono_tint.dctl");
        const QVariantMap statics {{k::DctlFile, file}, {k::dctlColor(1), QColor(255, 255, 128)}};
        const Px a = onSwatch(*m_gpu, QColor(100, 150, 50), make(fxid::Dctl, {}, statics));
        // luma 132.15 (Rec.709 weights); the tint is (1, 1, 0.502)
        QVERIFY2(std::abs(a.r - 132) <= 3 && std::abs(a.g - 132) <= 3 && std::abs(a.b - 66) <= 3,
            qPrintable(QStringLiteral("%1 %2 %3").arg(a.r).arg(a.g).arg(a.b)));
        // Combo box one step up: Rec.601 weights, luma 123.65
        const Px b = onSwatch(
            *m_gpu, QColor(100, 150, 50), make(fxid::Dctl, {{k::dctlParam(1), 1.0}}, statics));
        QVERIFY2(std::abs(b.r - 124) <= 3 && std::abs(b.g - 124) <= 3, qPrintable(QString::number(b.r)));
        // Amount (slot 2) 1 - 1 = 0: the picture itself
        const Px c0 = onSwatch(
            *m_gpu, QColor(100, 150, 50), make(fxid::Dctl, {{k::dctlParam(2), -1.0}}, statics));
        QVERIFY(std::abs(c0.r - 100) <= 2 && std::abs(c0.g - 150) <= 2 && std::abs(c0.b - 50) <= 2);
    }

    void dctlErrorsLeavePictureAlone()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("bad.dctl")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("__DEVICE__ float3 transform(int p_Width, int p_Height, int p_X, int p_Y, float p_R, float p_G, float p_B)\n"
                "{ float* q; return make_float3(p_R, p_G, p_B); }\n");
        f.close();
        const Px p = onSwatch(*m_gpu, QColor(90, 160, 220),
            make(fxid::Dctl, {}, {{k::DctlFile, f.fileName()}}));
        QVERIFY(std::abs(p.r - 90) <= 1 && std::abs(p.g - 160) <= 1 && std::abs(p.b - 220) <= 1);
        // A missing file as well
        const Px q = onSwatch(*m_gpu, QColor(90, 160, 220),
            make(fxid::Dctl, {}, {{k::DctlFile, dir.filePath(QStringLiteral("none.dctl"))}}));
        QVERIFY(std::abs(q.r - 90) <= 1);
    }

    void dctlTransitionOnGpu()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(64, 36);
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(200, 0, 0), 0, 50));
        ClipData b = solidClip(tl, QColor(0, 0, 200), 50, 50);
        TransitionData t;
        t.type = QLatin1String(fxid::DctlTransition);
        t.duration = 25;
        t.params = EffectRegistry::instance().defaultParams(t.type);
        t.params.setStatic(k::DctlFile, exampleDir() + QStringLiteral("/push_slide.dctl"));
        b.transitionIn = t;
        addClip(tl, TrackType::Video, 0, b);
        // progress 0.5 at frame 50: the left half still shows the outgoing picture, the right the incoming
        const QImage img = render(*m_gpu, tl, 50);
        const Px left = px(img, 10, 18), right = px(img, 54, 18);
        QVERIFY2(left.r > 150 && left.b < 50, qPrintable(QStringLiteral("%1 %2 %3").arg(left.r).arg(left.g).arg(left.b)));
        QVERIFY2(right.b > 150 && right.r < 50, qPrintable(QStringLiteral("%1 %2 %3").arg(right.r).arg(right.g).arg(right.b)));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestColorManagement>(argc, argv);
}

#include "test_colormanagement.moc"
