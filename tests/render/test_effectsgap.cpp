// SPDX-License-Identifier: GPL-3.0-or-later
// The 1.2 generators and effects (docs/behavior-spec.md section 56): test patterns against the
// published numbers (ITU-R BT.2111 Tables 2 to 4, EBU bars, 100 mV steps, Rec. 709 YCbCr), GPU
// against CPU for every new generator, the pointwise effects against their CPU reference,
// direction-of-change checks of the spatial effects, the Effects-section templates, and a
// project round trip. GPU cases skip without OpenGL.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Mask.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "render/Compositor.h"
#include "render/EffectMath.h"
#include "render/EffectMathExtra.h"
#include "render/Generators.h"
#include "render/GlEffects.h"
#include "render/TestPatterns.h"
#include "ui/effects/EffectActions.h"

#include <QTemporaryDir>

#include <set>

using namespace kk;
using namespace kk::test;
using namespace kk::render;

namespace {

namespace g = generators;
namespace k = fxkeys;
namespace tp = testpattern;

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

TimelineData single(const QString& id, int w = kW, int h = kH)
{
    TimelineData tl = emptyTimeline(w, h);
    addClip(tl, TrackType::Video, 0, generatorClip(tl, id));
    return tl;
}

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

/// Pixels that differ by more than @p tolerance in any channel.
int countDifferent(const QImage& a, const QImage& b, int tolerance)
{
    int n = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            n += maxDifference(a.copy(x, y, 1, 1), b.copy(x, y, 1, 1)) > tolerance ? 1 : 0;
    return n;
}

int to8(float level)
{
    return int(std::lround(level * 255.f));
}

/// Pixel at reference position (@p rx, @p ry) of a 1920 x 1080 frame in @p img of any size.
Px atReference(const QImage& img, double rx, double ry)
{
    return px(img, std::min(img.width() - 1, int(rx / 1920.0 * img.width())),
        std::min(img.height() - 1, int(ry / 1080.0 * img.height())));
}

/// Moves the clip ids of a template clip out of 0 so it can be placed in a test timeline.
ClipData placed(TimelineData& tl, ClipData clip)
{
    clip.id = tl.allocateId();
    for (EffectInstance& e : clip.effects)
        e.id = tl.allocateId();
    for (MaskData& m : clip.masks)
        m.id = tl.allocateId();
    clip.start = 0;
    clip.duration = 100;
    return clip;
}

/// The new generators: id, expected parameter keys.
const std::pair<const char*, QStringList> kGenerators[] = {
    {"steps_100mv", {g::Orientation}},
    {"ebu_color_bars", {}},
    {"bt2111_hlg_narrow", {}},
    {"bt2111_pq_narrow", {}},
    {"bt2111_pq_full", {}},
    {"window",
        {g::Shape, g::Width, g::Height, g::CenterX, g::CenterY, g::Softness, g::Color1, g::Color2}},
    {"ycbcr_ramp", {g::Channel, g::Orientation}},
    {"noise_gradient",
        {g::Color1, g::Color2, g::Scale, g::Detail, g::Contrast, g::Evolution, g::Seed}},
    {"grid",
        {g::Columns, g::Rows, g::LineWidth, g::LineColor, g::MajorEvery, g::MajorWidth,
            g::Background, g::BackgroundOpacity}},
};

/// The new video effects: id, folder.
const std::pair<const char*, const char*> kEffects[] = {
    {fxid::ColoredBorder, effectgroups::Stylize},
    {fxid::Despill, effectgroups::Color},
    {fxid::AlphaMatteShrinkGrow, effectgroups::Key},
    {fxid::Transform, effectgroups::Transform},
    {fxid::Dent, effectgroups::Distort},
    {fxid::FastNoise, effectgroups::Texture},
    {fxid::GamutLimiter, effectgroups::Color},
    {fxid::ContrastPop, effectgroups::Color},
    {fxid::ChromaticAberrationFix, effectgroups::Color},
    {fxid::FalseColor, effectgroups::Color},
    {fxid::Dehaze, effectgroups::Color},
};

const QColor kSwatches[] = {QColor(200, 100, 50), QColor(230, 166, 128), QColor(30, 200, 90),
    QColor(0, 177, 64), QColor(0, 71, 187), QColor(128, 128, 128), QColor(20, 20, 20),
    QColor(240, 240, 240), QColor(90, 160, 220), QColor(250, 40, 160), QColor(0, 255, 0),
    QColor(255, 0, 255), QColor(10, 250, 120)};

} // namespace

class TestEffectsGap : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    QTemporaryDir m_dir;

    bool gpuReady() const { return m_gpu && m_gpu->isGpu(); }

    QImage render(Compositor& c, const TimelineData& tl, int64_t frame = 0)
    {
        return c.renderImage(snap(tl), frame);
    }

    /// Both backends in turn (the GPU only where there is one).
    template<typename Fn>
    void eachBackend(Fn&& fn)
    {
        fn(*m_cpu, "CPU");
        if (gpuReady())
            fn(*m_gpu, "GPU");
    }

    /// Colour bars with @p e on the clip, rendered on the GPU (spatial effects have no CPU path).
    QImage barsWith(
        const QVector<EffectInstance>& effects, int w = kW, int h = kH, int64_t frame = 0)
    {
        TimelineData tl = emptyTimeline(w, h);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 100));
        ClipData bars = generatorClip(tl, QStringLiteral("color_bars"));
        bars.title.setStatic(g::Style, QStringLiteral("full"));
        bars.effects = effects;
        addClip(tl, TrackType::Video, 1, std::move(bars));
        return render(*m_gpu, tl, frame);
    }

    /// A Window generator clip (120 x 80 px of grey 160 on grey 100, centred) with @p effects:
    /// a hard edge inside the layer itself (effects run before the clip's transform).
    QImage windowWith(const QVector<EffectInstance>& effects, int64_t frame = 0,
        const QColor& inside = QColor(160, 160, 160), const QColor& outside = QColor(100, 100, 100),
        double width = 120, double height = 80)
    {
        TimelineData tl = emptyTimeline(kW, kH);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 100));
        ClipData win = generatorClip(tl, g::Window);
        win.title.setBase(g::Width, width);
        win.title.setBase(g::Height, height);
        win.title.setStatic(g::Color1, inside);
        win.title.setStatic(g::Color2, outside);
        win.effects = effects;
        addClip(tl, TrackType::Video, 1, std::move(win));
        return render(*m_gpu, tl, frame);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
    }
    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    // ---- registry ----

    void generatorsAreRegistered_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QStringList>("keys");
        for (const auto& [id, keyList] : kGenerators)
            QTest::newRow(id) << QString::fromLatin1(id) << keyList;
    }
    void generatorsAreRegistered()
    {
        QFETCH(QString, id);
        QFETCH(QStringList, keys);
        const EffectDescriptor* d = EffectRegistry::instance().find(id);
        QVERIFY(d);
        QCOMPARE(d->category, EffectCategory::Generator);
        QCOMPARE(d->storage, QStringLiteral("title"));
        QVERIFY(!d->name.isEmpty() && !d->description.isEmpty());
        QCOMPARE(d->params.size(), keys.size());
        for (const QString& key : keys) {
            const ParamDescriptor* p = d->param(key);
            QVERIFY2(p, qPrintable(id + " " + key));
            if (p->isScalar())
                QVERIFY2(p->def >= p->min && p->def <= p->max, qPrintable(id + " " + key));
            if (p->type == ParamType::Choice)
                QVERIFY(p->choices.contains(p->defStatic.toString()));
        }
        // Names the library shows (Resolve's generator list, spec 56.1).
        const QStringList names {QStringLiteral("100mV Steps"), QStringLiteral("EBU Color Bar"),
            QStringLiteral("BT.2111 Color Bar HLG Narrow"),
            QStringLiteral("BT.2111 Color Bar PQ Narrow"),
            QStringLiteral("BT.2111 Color Bar PQ Full"), QStringLiteral("Window"),
            QStringLiteral("YCbCr Ramp"), QStringLiteral("Noise Gradient"), QStringLiteral("Grid")};
        QVERIFY2(names.contains(d->name), qPrintable(d->name));
    }

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
        // The new folders are listed, with their names.
        const QStringList folders = registry.groups(EffectCategory::VideoEffect);
        QVERIFY(folders.contains(QLatin1String(effectgroups::Texture)));
        QCOMPARE(EffectRegistry::groupName(effectgroups::Texture), QStringLiteral("Texture"));
        QCOMPARE(EffectRegistry::groupName(effectgroups::Revival), QStringLiteral("Revival"));
        for (const QString& id :
            {g::ColoredBorderClip, g::BinocularsClip, g::NightVisionClip, g::CompositionClip}) {
            const EffectDescriptor* d = registry.find(id);
            QVERIFY2(d && d->category == EffectCategory::Effects, qPrintable(id));
        }
    }

    // ---- BT.2111: the published numbers ----

    /// A few patches exactly as printed in BT.2111-3 Tables 2, 3 and 4 (10-bit R'G'B').
    void bt2111Tables_data()
    {
        QTest::addColumn<int>("variant");
        QTest::addColumn<int>("patch");
        QTest::addColumn<int>("r");
        QTest::addColumn<int>("g");
        QTest::addColumn<int>("b");
        using V = tp::Bt2111;
        auto row = [](const char* name, V v, int patch, int r, int g, int b) {
            QTest::newRow(name) << int(v) << patch << r << g << b;
        };
        row("hlg 40% grey", V::HlgNarrow, 0, 414, 414, 414);
        row("hlg 100% white", V::HlgNarrow, 1, 940, 940, 940);
        row("hlg 100% blue", V::HlgNarrow, 7, 64, 64, 940);
        row("hlg 75% yellow", V::HlgNarrow, 9, 721, 721, 64);
        row("hlg 75% magenta", V::HlgNarrow, 12, 721, 64, 721);
        row("hlg -7% step", V::HlgNarrow, 16, 4, 4, 4);
        row("hlg 50% step", V::HlgNarrow, 22, 502, 502, 502);
        row("hlg 109% step", V::HlgNarrow, 28, 1019, 1019, 1019);
        row("hlg -2% black", V::HlgNarrow, 30, 48, 48, 48);
        row("hlg +4% black", V::HlgNarrow, 32, 99, 99, 99);
        row("hlg 709 yellow", V::HlgNarrow, 33, 713, 719, 316);
        row("hlg 709 blue", V::HlgNarrow, 38, 227, 147, 702);
        row("pq narrow 58% white", V::PqNarrow, 8, 573, 573, 573);
        row("pq narrow 58% cyan", V::PqNarrow, 10, 64, 573, 573);
        row("pq narrow 709 red", V::PqNarrow, 37, 531, 351, 257);
        row("pq narrow 709 green", V::PqNarrow, 35, 474, 565, 368);
        row("pq full grey", V::PqFull, 0, 409, 409, 409);
        row("pq full 100% yellow", V::PqFull, 2, 1023, 1023, 0);
        row("pq full 58% white", V::PqFull, 8, 594, 594, 594);
        row("pq full 58% red", V::PqFull, 13, 594, 0, 0);
        row("pq full 10% step", V::PqFull, 18, 102, 102, 102);
        row("pq full 90% step", V::PqFull, 26, 921, 921, 921);
        row("pq full +2% black", V::PqFull, 31, 19, 19, 19);
        row("pq full +4% black", V::PqFull, 32, 41, 41, 41);
        row("pq full 709 magenta", V::PqFull, 36, 552, 348, 584);
        row("pq full 709 blue", V::PqFull, 38, 296, 201, 582);
    }
    void bt2111Tables()
    {
        QFETCH(int, variant);
        QFETCH(int, patch);
        QFETCH(int, r);
        QFETCH(int, g);
        QFETCH(int, b);
        const auto code = tp::bt2111Code(tp::Bt2111(variant), patch);
        QCOMPARE(code[0], r);
        QCOMPARE(code[1], g);
        QCOMPARE(code[2], b);
    }

    /// Signal levels: narrow range 64..940 is 0..100 %, so 75 % HLG white (721) is 75 % and the
    /// 58 % PQ white (573) is 58 %; full range is code / 1023.
    void bt2111Levels()
    {
        using V = tp::Bt2111;
        QVERIFY(std::abs(tp::bt2111Level(V::HlgNarrow, 721) - 0.75f) < 0.001f);
        QVERIFY(std::abs(tp::bt2111Level(V::PqNarrow, 573) - 0.581f) < 0.001f);
        QCOMPARE(tp::bt2111Level(V::HlgNarrow, 940), 1.0f);
        QCOMPARE(tp::bt2111Level(V::HlgNarrow, 64), 0.0f);
        QCOMPARE(tp::bt2111Level(V::HlgNarrow, 4), 0.0f); // -7 % clips on a full-range canvas
        QCOMPARE(tp::bt2111Level(V::HlgNarrow, 1019), 1.0f);
        QVERIFY(std::abs(tp::bt2111Level(V::PqFull, 594) - 0.5806f) < 0.001f);
        QCOMPARE(tp::bt2111Level(V::PqFull, 1023), 1.0f);
        // Ramp tables: HLG / PQ narrow B = 559 at 4, C = 1014 from 5 to 1018, D = 107 at 1019;
        // PQ full B = 618 at 0, C = 1022 from 1 to 1022, D = 40 at 1023 (Tables 5 and 6).
        QCOMPARE(tp::bt2111RampCode(V::HlgNarrow, 240), 4);
        QCOMPARE(tp::bt2111RampCode(V::HlgNarrow, 240 + 558), 4);
        QCOMPARE(tp::bt2111RampCode(V::HlgNarrow, 240 + 559), 5);
        QCOMPARE(tp::bt2111RampCode(V::HlgNarrow, 240 + 559 + 1013), 1018);
        QCOMPARE(tp::bt2111RampCode(V::HlgNarrow, 240 + 559 + 1014), 1019);
        QCOMPARE(tp::bt2111RampCode(V::HlgNarrow, 1919), 1019);
        QCOMPARE(tp::bt2111RampCode(V::PqFull, 240 + 617), 0);
        QCOMPARE(tp::bt2111RampCode(V::PqFull, 240 + 618), 1);
        QCOMPARE(tp::bt2111RampCode(V::PqFull, 240 + 618 + 1021), 1022);
        QCOMPARE(tp::bt2111RampCode(V::PqFull, 1919), 1023);
        // The ramp reaches 0 % (code 64) at the left edge of the green bar (x = 858).
        QCOMPARE(tp::bt2111RampCode(V::HlgNarrow, 858), 64);
    }

    /// The layout of Table 1 (a..k): patch under reference pixels of every region.
    void bt2111Layout()
    {
        QCOMPARE(tp::bt2111PatchAt(100, 400), 0); // 40 % grey, left
        QCOMPARE(tp::bt2111PatchAt(1800, 400), 0); // and right
        QCOMPARE(tp::bt2111PatchAt(300, 40), 1); // 100 % white
        QCOMPARE(tp::bt2111PatchAt(300, 300), 8); // 75 % white
        QCOMPARE(tp::bt2111PatchAt(1500, 300), 14); // blue (x 1474 .. 1679)
        QCOMPARE(tp::bt2111PatchAt(1000, 40), 4); // green is d = 204 wide: 858 .. 1061
        QCOMPARE(tp::bt2111PatchAt(1061, 300), 11);
        QCOMPARE(tp::bt2111PatchAt(1062, 300), 12);
        QCOMPARE(tp::bt2111PatchAt(300, 650), 16); // -7 % step under white
        QCOMPARE(tp::bt2111PatchAt(500, 650), 17); // 0 % under yellow, left half
        QCOMPARE(tp::bt2111PatchAt(600, 650), 18); // 10 %, right half
        QCOMPARE(tp::bt2111PatchAt(1650, 650), 28); // 109 % under blue, right half
        QCOMPARE(tp::bt2111PatchAt(100, 650), 8); // 75 % white left of the stairs
        QCOMPARE(tp::bt2111PatchAt(100, 760), 29); // 0 % black left of the ramp
        QCOMPARE(tp::bt2111PatchAt(900, 760), -1); // ramp
        QCOMPARE(tp::bt2111PatchAt(30, 900), 33); // 75 % BT.709 yellow
        QCOMPARE(tp::bt2111PatchAt(200, 900), 35);
        QCOMPARE(tp::bt2111PatchAt(400, 900), 30); // f = 136, then -2 % black (g = 70)
        QCOMPARE(tp::bt2111PatchAt(700, 900), 32); // +4 % black
        QCOMPARE(tp::bt2111PatchAt(1200, 900), 8); // 75 % white block j
        QCOMPARE(tp::bt2111PatchAt(1700, 900), 36); // BT.709 magenta, right
        QCOMPARE(tp::bt2111PatchAt(1900, 900), 38);
    }

    void bt2111Renders_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<int>("variant");
        QTest::newRow("hlg") << g::Bt2111HlgNarrow << int(tp::Bt2111::HlgNarrow);
        QTest::newRow("pq narrow") << g::Bt2111PqNarrow << int(tp::Bt2111::PqNarrow);
        QTest::newRow("pq full") << g::Bt2111PqFull << int(tp::Bt2111::PqFull);
    }
    void bt2111Renders()
    {
        QFETCH(QString, id);
        QFETCH(int, variant);
        const auto v = tp::Bt2111(variant);
        const TimelineData tl = single(id, 960, 540);
        eachBackend([&](Compositor& c, const char* name) {
            const QImage img = render(c, tl);
            // Centre of every palette patch drawn at a known reference position.
            struct At
            {
                int patch;
                double x, y;
            };
            const At spots[] = {{0, 100, 400}, {1, 340, 40}, {4, 960, 40}, {8, 340, 300},
                {10, 750, 300}, {13, 1400, 300}, {14, 1580, 300}, {16, 340, 670}, {22, 1000, 670},
                {28, 1650, 670}, {29, 100, 760}, {30, 410, 950}, {31, 550, 950}, {32, 690, 950},
                {33, 40, 950}, {36, 1720, 950}, {38, 1880, 950}};
            for (const At& s : spots) {
                const Px p = atReference(img, s.x, s.y);
                const auto& pal = tp::bt2111Palette(v)[size_t(s.patch)];
                QVERIFY2(near(p, to8(pal.r), to8(pal.g), to8(pal.b), 255, 1),
                    qPrintable(QStringLiteral("%1 patch %2 at %3,%4: %5")
                                   .arg(QLatin1String(name))
                                   .arg(s.patch)
                                   .arg(s.x)
                                   .arg(s.y)
                                   .arg(QString::fromLatin1(describe(p)))));
            }
            // The ramp rises left to right from the first bar to the last, in grey.
            const Px a = atReference(img, 1000, 760), b = atReference(img, 1500, 760);
            QVERIFY(a.r == a.g && a.g == a.b && b.r == b.g && b.g == b.b);
            QVERIFY2(b.r > a.r, name);
        });
    }

    // ---- EBU bars, 100 mV steps, YCbCr ramp, window, grid, noise gradient ----

    void ebuBars()
    {
        // EBU Tech 3213 / BT.471 100/0/75/0: white 100 %, colours 75 % (191 of 255), black 0.
        const int expected[8][3] = {{255, 255, 255}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
            {191, 0, 191}, {191, 0, 0}, {0, 0, 191}, {0, 0, 0}};
        for (int i = 0; i < 8; ++i)
            QCOMPARE(tp::ebuBarsAt((i + 0.5) / 8.0),
                qRgb(expected[i][0], expected[i][1], expected[i][2]));
        const TimelineData tl = single(g::EbuBars);
        eachBackend([&](Compositor& c, const char* name) {
            const QImage img = render(c, tl);
            for (int i = 0; i < 8; ++i) {
                for (const int y : {5, 90, 175}) { // full height: no bottom row
                    const Px p = px(img, int((i + 0.5) * kW / 8), y);
                    QVERIFY2(near(p, expected[i][0], expected[i][1], expected[i][2], 255, 1), name);
                }
            }
        });
    }

    void steps100mV()
    {
        // Eight steps of 100 mV of a 700 mV signal: 0, 100 ... 700 mV = n / 7 of full scale.
        TimelineData tl = single(g::Steps100mV, 320, 180);
        eachBackend([&](Compositor& c, const char* name) {
            const QImage img = render(c, tl);
            for (int n = 0; n < 8; ++n) {
                const int level = int(std::lround(255.0 * n / 7.0));
                const Px p = px(img, n * 40 + 20, 90);
                QVERIFY2(near(p, level, level, level, 255, 1), name);
            }
        });
        tl.videoTracks[0].clips[0].title.setStatic(g::Orientation, QStringLiteral("vertical"));
        const QImage v = render(*m_cpu, tl);
        QVERIFY(near(px(v, 160, 5), 0, 0, 0, 255, 1));
        QVERIFY(near(px(v, 160, 175), 255, 255, 255, 255, 1));
    }

    void ycbcrRamp()
    {
        // Rec. 709 (Kr = 0.2126, Kb = 0.0722): Cb = -0.5 at Y' = 0.5 is R 0.5, G 0.5937, B 0.
        const tp::Rgbf cbLow = tp::ycbcrToRgb(0.5f, -0.5f, 0.f);
        QVERIFY(std::abs(cbLow.r - 0.5f) < 1e-4f && std::abs(cbLow.g - 0.59366f) < 1e-3f
            && cbLow.b == 0.f);
        const tp::Rgbf crHigh = tp::ycbcrToRgb(0.5f, 0.f, 0.5f);
        QVERIFY(crHigh.r == 1.f && std::abs(crHigh.g - 0.26594f) < 1e-3f
            && std::abs(crHigh.b - 0.5f) < 1e-4f);
        QCOMPARE(tp::ycbcrToRgb(0.3f, 0.f, 0.f).g, 0.3f);
        TimelineData tl = single(g::YcbcrRamp);
        eachBackend([&](Compositor& c, const char* name) {
            const QImage img = render(c, tl);
            // Three bands stacked: Y' (grey), Cb, Cr; each ramps along x.
            QVERIFY2(near(px(img, 0, 30), 0, 0, 0, 255, 2), name);
            QVERIFY2(near(px(img, 319, 30), 255, 255, 255, 255, 2), name);
            const Px cb0 = px(img, 0, 90), cb1 = px(img, 319, 90);
            QVERIFY2(near(cb0, to8(cbLow.r), to8(cbLow.g), to8(cbLow.b), 255, 2), name);
            QVERIFY2(cb1.b > 250 && cb1.r < 135, name); // Cb up: blue
            const Px cr1 = px(img, 319, 150);
            QVERIFY2(cr1.r > 250 && cr1.b < 135, name); // Cr up: red
        });
        tl.videoTracks[0].clips[0].title.setStatic(g::Channel, QStringLiteral("cr"));
        const QImage single = render(*m_cpu, tl);
        QVERIFY(px(single, 319, 10).r > 250 && px(single, 319, 170).r > 250);
    }

    void windowGenerator()
    {
        TimelineData tl = single(g::Window);
        eachBackend([&](Compositor& c, const char* name) {
            const QImage img = render(c, tl);
            // The default 960 x 540 window is made for a 1080p frame: it covers this small one.
            QVERIFY2(near(px(img, 160, 90), 255, 255, 255, 255, 1), name);
        });
        // Use a window of 160 x 90 timeline pixels, shifted, as an ellipse with softness.
        ClipData& clip = tl.videoTracks[0].clips[0];
        clip.title.setBase(g::Width, 160);
        clip.title.setBase(g::Height, 90);
        clip.title.setBase(g::CenterX, 40);
        eachBackend([&](Compositor& c, const char* name) {
            const QImage img = render(c, tl);
            QVERIFY2(near(px(img, 200, 90), 255, 255, 255, 255, 1), name); // centre x = 160 + 40
            QVERIFY2(near(px(img, 100, 90), 0, 0, 0, 255, 1), name); // left of the window
            QVERIFY2(near(px(img, 200, 40), 0, 0, 0, 255, 1), name); // above it (y 45 .. 134)
            QVERIFY2(near(px(img, 200, 50), 255, 255, 255, 255, 1), name);
            QVERIFY2(near(px(img, 118, 90), 0, 0, 0, 255, 1), name); // window spans x 120 .. 279
            QVERIFY2(near(px(img, 122, 90), 255, 255, 255, 255, 1), name);
        });
        clip.title.setStatic(g::Shape, QStringLiteral("ellipse"));
        eachBackend([&](Compositor& c, const char* name) {
            const QImage img = render(c, tl);
            QVERIFY2(near(px(img, 200, 90), 255, 255, 255, 255, 1), name);
            QVERIFY2(near(px(img, 124, 48), 0, 0, 0, 255, 1), name); // a corner of the box
        });
        clip.title.setBase(g::Softness, 40);
        eachBackend([&](Compositor& c, const char* name) {
            const QImage img = render(c, tl);
            const int edge = qGray(img.pixel(279, 90)); // on the ellipse edge: half
            QVERIFY2(edge > 100 && edge < 160, qPrintable(QString(name) + QString::number(edge)));
        });
        // The colours are parameters.
        clip.title.setStatic(g::Color1, QColor(255, 0, 0));
        clip.title.setStatic(g::Color2, QColor(0, 0, 255));
        QVERIFY(near(px(render(*m_cpu, tl), 200, 90), 255, 0, 0, 255, 1));
        QVERIFY(near(px(render(*m_cpu, tl), 5, 5), 0, 0, 255, 255, 1));
    }

    void gridGenerator()
    {
        TimelineData tl = emptyTimeline(kW, kH);
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(200, 0, 0), 0, 100));
        ClipData grid = generatorClip(tl, g::Grid);
        grid.title.setBase(g::Columns, 8); // 40 px cells
        grid.title.setBase(g::Rows, 6); // 30 px cells
        grid.title.setBase(g::LineWidth, 2);
        grid.title.setBase(g::MajorWidth, 6);
        grid.title.setBase(g::MajorEvery, 4);
        addClip(tl, TrackType::Video, 1, grid);
        eachBackend([&](Compositor& c, const char* name) {
            const QImage img = render(c, tl);
            // Minor line at x = 40 (white, 2 wide), major lines at x = 0, 160 (6 wide).
            QVERIFY2(near(px(img, 40, 15), 255, 255, 255, 255, 2), name);
            QVERIFY2(near(px(img, 39, 15), 255, 255, 255, 255, 2), name);
            QVERIFY2(near(px(img, 41, 15), 200, 0, 0, 255, 2), name); // line is x 39..40
            QVERIFY2(near(px(img, 20, 15), 200, 0, 0, 255, 1), name); // see-through in a cell
            QVERIFY2(near(px(img, 158, 15), 255, 255, 255, 255, 2), name); // major, 3 px each side
            QVERIFY2(near(px(img, 162, 15), 255, 255, 255, 255, 2), name);
            QVERIFY2(near(px(img, 164, 15), 200, 0, 0, 255, 2), name);
            QVERIFY2(near(px(img, 20, 30), 255, 255, 255, 255, 2), name); // row line y = 30
            QVERIFY2(near(px(img, 20, 15), 200, 0, 0, 255, 1), name);
        });
        // An opaque background hides the picture below.
        tl.videoTracks[1].clips[0].title.setBase(g::BackgroundOpacity, 100);
        QVERIFY(near(px(render(*m_cpu, tl), 20, 15), 0, 0, 0, 255, 1));
    }

    void noiseGradientGenerator()
    {
        TimelineData tl = single(g::NoiseGradient);
        ClipData& clip = tl.videoTracks[0].clips[0];
        const QImage a = render(*m_cpu, tl);
        QCOMPARE(maxDifference(a, render(*m_cpu, tl)), 0);
        // Smooth: neighbours differ little; a cloud: the picture is not flat.
        int steepest = 0, lo = 255, hi = 0;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x + 1 < kW; ++x) {
                steepest
                    = std::max(steepest, std::abs(qGray(a.pixel(x, y)) - qGray(a.pixel(x + 1, y))));
                lo = std::min(lo, qGray(a.pixel(x, y)));
                hi = std::max(hi, qGray(a.pixel(x, y)));
            }
        }
        QVERIFY2(steepest < 12, qPrintable(QString::number(steepest)));
        QVERIFY2(hi - lo > 80, qPrintable(QString::number(hi - lo)));
        // Seed, evolution and scale change the picture; both colours are used.
        TimelineData other = tl;
        other.videoTracks[0].clips[0].title.setBase(g::Seed, 9);
        QVERIFY(maxDifference(a, render(*m_cpu, other)) > 60);
        other = tl;
        other.videoTracks[0].clips[0].title.setBase(g::Evolution, 3.5);
        QVERIFY(maxDifference(a, render(*m_cpu, other)) > 60);
        other = tl;
        other.videoTracks[0].clips[0].title.setBase(g::Scale, 40);
        QVERIFY(steepest < [&] {
            const QImage b = render(*m_cpu, other);
            int s = 0;
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x + 1 < kW; ++x)
                    s = std::max(s, std::abs(qGray(b.pixel(x, y)) - qGray(b.pixel(x + 1, y))));
            return s;
        }());
        // The pixel is the documented formula: value noise, contrast about the middle.
        const float n = tp::noiseGradientValue(100.5f, 50.5f, 400.f, 4, 0.f, 1);
        const int expected = to8(std::clamp((n - 0.5f) * 2.f + 0.5f, 0.f, 1.f));
        QVERIFY(std::abs(qGray(a.pixel(100, 50)) - expected) <= 1);
        // Keyframes: Evolution animates.
        AnimatedValue& evo = clip.title.animated(g::Evolution, 0);
        evo.setKey(0, 0.0);
        evo.setKey(20, 4.0);
        QVERIFY(maxDifference(render(*m_cpu, tl, 0), render(*m_cpu, tl, 20)) > 60);
    }

    /// Every new generator: GPU and CPU agree and the picture is deterministic.
    void gpuMatchesCpu_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<int>("tolerance");
        QTest::newRow("steps") << g::Steps100mV << 1;
        QTest::newRow("ebu") << g::EbuBars << 1;
        QTest::newRow("hlg") << g::Bt2111HlgNarrow << 1;
        QTest::newRow("pq narrow") << g::Bt2111PqNarrow << 1;
        QTest::newRow("pq full") << g::Bt2111PqFull << 1;
        QTest::newRow("window") << g::Window << 1;
        QTest::newRow("ycbcr") << g::YcbcrRamp << 2;
        QTest::newRow("noise gradient") << g::NoiseGradient << 3;
        QTest::newRow("grid") << g::Grid << 2;
    }
    void gpuMatchesCpu()
    {
        QFETCH(QString, id);
        QFETCH(int, tolerance);
        const TimelineData tl = single(id);
        const QImage cpu = render(*m_cpu, tl, 7);
        QCOMPARE(maxDifference(cpu, render(*m_cpu, tl, 7)), 0);
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QImage gpu = render(*m_gpu, tl, 7);
        QCOMPARE(maxDifference(gpu, render(*m_gpu, tl, 7)), 0);
        // Single-pixel rounding at pattern edges is allowed, a visible difference is not.
        const int off = countDifferent(gpu, cpu, tolerance);
        QVERIFY2(off < kW * kH / 200, qPrintable(id + ": " + QString::number(off)));
    }

    // ---- pointwise effects: GPU against the CPU reference ----

    void pointEffectsMatchReference_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QVariantMap>("values");
        QTest::addColumn<QVariantMap>("statics");
        auto row = [](const char* name, const char* type, QVariantMap v, QVariantMap s = {}) {
            QTest::newRow(name) << QString::fromLatin1(type) << v << s;
        };
        row("despill green", fxid::Despill, {{k::Amount, 100.0}});
        row("despill blue max", fxid::Despill, {{k::Amount, 70.0}},
            {{k::Screen, fxchoice::Blue}, {k::SpillMethod, fxchoice::Maximum}});
        row("despill red", fxid::Despill, {{k::Amount, 100.0}}, {{k::Screen, fxchoice::Red}});
        row("gamut 2020 to 709", fxid::GamutLimiter, {});
        row("gamut clip", fxid::GamutLimiter, {{k::Amount, 80.0}}, {{k::Mode, fxchoice::Clip}});
        row("false color exposure", fxid::FalseColor, {});
        row("false color thermal", fxid::FalseColor, {{k::Amount, 75.0}},
            {{k::Palette, fxchoice::Thermal}});
        row("false color rainbow", fxid::FalseColor, {}, {{k::Palette, fxchoice::Rainbow}});
        row("gamut p3 to 709 gamma 2.2", fxid::GamutLimiter, {{k::Gamma, 2.2}},
            {{k::SourceGamut, fxchoice::DciP3}});
    }
    void pointEffectsMatchReference()
    {
        QFETCH(QString, type);
        QFETCH(QVariantMap, values);
        QFETCH(QVariantMap, statics);
        const QColor background(20, 30, 200);
        int worst = 0;
        eachBackend([&](Compositor& c, const char* name) {
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
                QImage expected(1, 1, QImage::Format_ARGB32_Premultiplied);
                expected.fill(colour);
                QVERIFY(fxmath::applyPointEffect(type, e.params, 0, expected, 25.0));
                addClip(tl, TrackType::Video, 1, std::move(fg));
                const Px out = px(render(c, tl), 32, 18);
                const QRgb want = expected.pixel(0, 0);
                const int diff = std::max({std::abs(out.r - qRed(want)),
                    std::abs(out.g - qGreen(want)), std::abs(out.b - qBlue(want))});
                worst = std::max(worst, diff);
                QVERIFY2(diff <= 3,
                    qPrintable(QStringLiteral("%1 %2 on %3: got %4")
                                   .arg(QLatin1String(name), type, colour.name(),
                                       QString::fromLatin1(describe(out)))));
            }
        });
        qInfo("%s: largest difference to the reference %d", qPrintable(type), worst);
    }

    void despillAndGamutMath()
    {
        // Despill: a green-cast skin tone loses the green above the red / blue mean.
        ParamSet p = EffectRegistry::instance().defaultParams(QLatin1String(fxid::Despill));
        const fxmath::Rgb out
            = fxmath::despillColor(fxmath::despillParams(p, 0), {0.5f, 0.8f, 0.3f});
        QVERIFY(std::abs(out.g - 0.4f) < 1e-5f && out.r == 0.5f && out.b == 0.3f);
        p.setBase(k::Amount, 0.0);
        QCOMPARE(fxmath::despillColor(fxmath::despillParams(p, 0), {0.5f, 0.8f, 0.3f}).g, 0.8f);
        // Gamut limiter: grey and 709-safe colours stay; Rec. 2020 green is pulled in and
        // still has the same hue family (green dominant) and less saturation.
        const ParamSet gp
            = EffectRegistry::instance().defaultParams(QLatin1String(fxid::GamutLimiter));
        const fxmath::GamutLimiterParams lim = fxmath::gamutLimiterParams(gp, 0);
        const fxmath::Rgb grey = fxmath::gamutLimit(lim, {0.4f, 0.4f, 0.4f});
        QVERIFY(grey.r == 0.4f && grey.g == 0.4f && grey.b == 0.4f);
        const fxmath::Rgb pure = fxmath::gamutLimit(lim, {0.f, 1.f, 0.f});
        QVERIFY(pure.g > pure.r && pure.g > pure.b);
        QVERIFY(pure.r > 0.3f && pure.b > 0.3f); // desaturated
        // The limited colour lies inside the Rec. 709 gamut: converting back gives no negatives.
        ParamSet same = gp;
        same.setStatic(k::SourceGamut, fxchoice::Rec709);
        const fxmath::Rgb unchanged
            = fxmath::gamutLimit(fxmath::gamutLimiterParams(same, 0), {0.f, 1.f, 0.f});
        QCOMPARE(unchanged.g, 1.f); // 709 into 709: nothing to limit
        // Fast noise blend modes at the end points.
        const fxmath::Rgb base {0.25f, 0.5f, 0.75f}, noise {0.5f, 0.5f, 0.5f};
        QCOMPARE(fxmath::blendNoise(0, 1.f, base, noise).r, 0.5f); // normal
        QCOMPARE(fxmath::blendNoise(1, 1.f, base, noise).r, 0.75f); // add
        QCOMPARE(fxmath::blendNoise(2, 1.f, base, noise).r, 0.125f); // multiply
        QCOMPARE(fxmath::blendNoise(3, 1.f, base, noise).r, 0.625f); // screen
        QCOMPARE(fxmath::blendNoise(4, 1.f, base, noise).r, 0.25f); // overlay (2 * 0.25 * 0.5)
        QCOMPARE(fxmath::blendNoise(0, 0.f, base, noise).r, 0.25f); // blend 0 = untouched
    }

    // ---- spatial effects: they change the picture in the expected direction ----

    void neutralParametersLeavePictureAlone()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        const QImage plain = barsWith({});
        TimelineData t = emptyTimeline();
        struct Row
        {
            const char* type;
            std::initializer_list<std::pair<const QString, double>> values;
        };
        const Row rows[]
            = {{fxid::ColoredBorder, {{k::Opacity, 0.0}}}, {fxid::ColoredBorder, {{k::Width, 0.0}}},
                {fxid::Despill, {{k::Amount, 0.0}}}, {fxid::AlphaMatteShrinkGrow, {}},
                {fxid::Transform, {}}, {fxid::Dent, {{k::Amount, 0.0}}},
                {fxid::FastNoise, {{k::Amount, 0.0}}}, {fxid::GamutLimiter, {{k::Amount, 0.0}}},
                {fxid::ContrastPop, {{k::Amount, 0.0}}}, {fxid::FalseColor, {{k::Amount, 0.0}}},
                {fxid::Dehaze, {{k::Amount, 0.0}}}, {fxid::ChromaticAberrationFix, {}}};
        for (const Row& r : rows) {
            const QImage img = barsWith({effect(t, r.type, r.values)});
            QVERIFY2(maxDifference(img, plain) <= 1, r.type);
        }
        // A Gamut Limiter whose picture and limit gamuts are the same has nothing to limit.
        QVERIFY(
            maxDifference(
                barsWith({effect(t, fxid::GamutLimiter, {}, {{k::SourceGamut, fxchoice::Rec709}})}),
                plain)
            <= 3);
        // ... and the default (Rec. 2020 picture into Rec. 709) changes saturated colors.
        QVERIFY(maxDifference(barsWith({effect(t, fxid::GamutLimiter)}), plain) > 10);
    }

    void coloredBorder()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QVariant amber = QColor(255, 128, 0);
        // The border runs inside the edge of the layer: here the Window clip's picture, which
        // fills the frame (inside grey 160 at the centre, 100 outside the 120 x 80 window).
        const QImage plain = windowWith({});
        const QImage img
            = windowWith({effect(t, fxid::ColoredBorder, {{k::Width, 20.0}}, {{k::Color, amber}})});
        QVERIFY(near(px(img, 160, 90), 160, 160, 160, 255, 1)); // centre untouched
        QVERIFY(near(px(img, 10, 90), 255, 128, 0, 255, 2)); // 10 px inside the left edge
        QVERIFY(near(px(img, 160, 10), 255, 128, 0, 255, 2)); // and the top
        QVERIFY(near(px(img, 310, 170), 255, 128, 0, 255, 2)); // the corner
        QVERIFY(near(px(img, 30, 90), 100, 100, 100, 255, 1)); // beyond the border: picture
        // Rounded corners: the inner corner pixel (21, 21) is picture without rounding and
        // border with it.
        const QImage round = windowWith({effect(
            t, fxid::ColoredBorder, {{k::Width, 20.0}, {k::Radius, 40.0}}, {{k::Color, amber}})});
        QVERIFY(near(px(img, 22, 22), 100, 100, 100, 255, 1));
        QVERIFY(near(px(round, 22, 22), 255, 128, 0, 255, 2));
        // Feather softens the inner edge: a pixel just inside it is a mix.
        const QImage soft = windowWith({effect(
            t, fxid::ColoredBorder, {{k::Width, 20.0}, {k::Feather, 12.0}}, {{k::Color, amber}})});
        const Px hard = px(img, 22, 90), blurred = px(soft, 22, 90);
        QVERIFY2(
            hard.r == 100 && blurred.r > 110 && blurred.r < 250, describe(blurred).constData());
        QVERIFY(maxDifference(plain, img) > 40);
        const QImage half = windowWith({effect(
            t, fxid::ColoredBorder, {{k::Width, 20.0}, {k::Opacity, 50.0}}, {{k::Color, amber}})});
        QVERIFY(near(px(half, 10, 90), 178, 114, 50, 255, 3));
    }

    void alphaMatteShrinkGrow()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        // A Grid clip: opaque white lines 20 px wide on a see-through background (the effect
        // sees the layer before the clip's transform, so the alpha edge has to be in it).
        auto lines = [&](const QVector<EffectInstance>& effects) {
            TimelineData tl = emptyTimeline(kW, kH);
            addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 10));
            ClipData grid = generatorClip(tl, g::Grid);
            grid.title.setBase(g::Columns, 2); // one vertical line at x = 160
            grid.title.setBase(g::Rows, 1);
            grid.title.setBase(g::LineWidth, 20);
            grid.title.setBase(g::MajorEvery, 0);
            grid.effects = effects;
            addClip(tl, TrackType::Video, 1, std::move(grid));
            return render(*m_gpu, tl);
        };
        auto width = [](const QImage& img) {
            int w = 0;
            for (int x = 100; x < 220; ++x)
                w += qGray(img.pixel(x, 90)) > 128 ? 1 : 0;
            return w;
        };
        TimelineData t = emptyTimeline();
        auto choke = [&](double v) {
            return lines({effect(t, fxid::AlphaMatteShrinkGrow, {{k::ShrinkGrow, v}})});
        };
        const int base = width(lines({})), shrunk = width(choke(-4)), grown = width(choke(6));
        qInfo("line width: shrink %d, none %d, grow %d", shrunk, base, grown);
        QCOMPARE(base, 20);
        QVERIFY(std::abs(shrunk - 12) <= 2);
        QVERIFY(std::abs(grown - 32) <= 3);
        // Edge blur softens the line without moving its middle.
        const QImage soft = lines({effect(t, fxid::AlphaMatteShrinkGrow, {{k::EdgeBlur, 6.0}})});
        QVERIFY(qGray(soft.pixel(160, 90)) > 250);
        const int edge = qGray(soft.pixel(150, 90)); // the line's edge: half
        QVERIFY2(edge > 60 && edge < 200, qPrintable(QString::number(edge)));
        QVERIFY(qGray(soft.pixel(120, 90)) < 5);
    }

    void transformEffect()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage plain = barsWith({});
        // Position: the picture moves right by 40 px, the left strip becomes transparent (black
        // over the empty timeline).
        const QImage moved = barsWith({effect(t, fxid::Transform, {{k::OffsetX, 40.0}})});
        QVERIFY(near(px(moved, 20, 90), 0, 0, 0, 255, 1));
        for (const int x : {60, 100, 200, 250})
            QVERIFY(near(px(moved, x, 90), qRed(plain.pixel(x - 40, 90)),
                qGreen(plain.pixel(x - 40, 90)), qBlue(plain.pixel(x - 40, 90)), 255, 3));
        // Scale 50 %: the centre stays, the corner shows the empty timeline.
        const QImage small = barsWith({effect(t, fxid::Transform, {{k::Scale, 50.0}})});
        QVERIFY(near(px(small, 5, 5), 0, 0, 0, 255, 1));
        QVERIFY(near(px(small, 160, 90), qRed(plain.pixel(160, 90)), qGreen(plain.pixel(160, 90)),
            qBlue(plain.pixel(160, 90)), 255, 3));
        // The bar at x 20..30 of the shrunk picture is the plain picture at x 40..60.
        QVERIFY(near(px(small, 120, 90), qRed(plain.pixel(80, 90)), qGreen(plain.pixel(80, 90)),
            qBlue(plain.pixel(80, 90)), 255, 3));
        // Rotation by 180 degrees mirrors the bars about the centre.
        const QImage turned = barsWith({effect(t, fxid::Transform, {{k::Rotation, 180.0}})});
        QVERIFY(near(px(turned, 300, 90), qRed(plain.pixel(19, 89)), qGreen(plain.pixel(19, 89)),
            qBlue(plain.pixel(19, 89)), 255, 3));
        // Rotation by +90 is clockwise: the part of the picture left of the centre (x 100)
        // goes to above it, the part to the right (x 220) below it.
        const QImage cw = barsWith({effect(t, fxid::Transform, {{k::Rotation, 90.0}})});
        for (const auto& [from, to] : {std::pair {QPoint(100, 90), QPoint(160, 30)},
                 std::pair {QPoint(220, 90), QPoint(160, 150)}}) {
            const QRgb want = plain.pixel(from);
            QVERIFY2(near(px(cw, to.x(), to.y()), qRed(want), qGreen(want), qBlue(want), 255, 3),
                "a clockwise quarter turn moves the left side to the top");
        }
        // Opacity 50 % blends with what is below.
        const QImage faint = barsWith({effect(t, fxid::Transform, {{k::Opacity, 50.0}})});
        QVERIFY(near(px(faint, 20, 90), qRed(plain.pixel(20, 90)) / 2,
            qGreen(plain.pixel(20, 90)) / 2, qBlue(plain.pixel(20, 90)) / 2, 255, 3));
    }

    void dentEffect()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage plain = barsWith({});
        // First bar edge right of the centre, in a row.
        // The dent sits at x = 180, inside the magenta bar (x 160 .. 200); its right edge is the
        // first change after x = 180. A bulge pushes that edge outward, a pinch pulls it in.
        auto edgeRight = [](const QImage& img, int y) {
            const QRgb centre = img.pixel(180, y);
            for (int x = 181; x < img.width(); ++x)
                if (img.pixel(x, y) != centre)
                    return x;
            return -1;
        };
        const int base = edgeRight(plain, 90);
        QVERIFY(base > 161);
        const QImage bulge
            = barsWith({effect(t, fxid::Dent, {{k::Amount, 80.0}, {k::Size, 150.0}})});
        const QImage pinch = barsWith({effect(
            t, fxid::Dent, {{k::Amount, 80.0}, {k::Size, 150.0}}, {{k::Mode, fxchoice::Pinch}})});
        qInfo("dent: first edge right of the centre: plain %d, bulge %d, pinch %d", base,
            edgeRight(bulge, 90), edgeRight(pinch, 90));
        QVERIFY(edgeRight(bulge, 90) > base + 3); // magnified: the edge moves out
        QVERIFY(edgeRight(pinch, 90) < base - 3);
        // Outside the size nothing moves.
        QVERIFY(near(px(bulge, 310, 90), qRed(plain.pixel(310, 90)), qGreen(plain.pixel(310, 90)),
            qBlue(plain.pixel(310, 90)), 255, 1));
        // The horizontal bow moves vertical edges only (a row far from the centre row is not
        // affected, a row at the centre is); the vertical bow leaves a vertical edge alone.
        const QImage bowH = barsWith(
            {effect(t, fxid::Dent, {{k::Amount, 80.0}, {k::Size, 60.0}, {k::CenterX, 20.0}},
                {{k::Mode, fxchoice::BowHorizontal}})});
        QVERIFY(edgeRight(bowH, 90) != base);
        QCOMPARE(edgeRight(bowH, 5), edgeRight(plain, 5));
        const QImage bowV = barsWith(
            {effect(t, fxid::Dent, {{k::Amount, 80.0}, {k::Size, 60.0}, {k::CenterX, 20.0}},
                {{k::Mode, fxchoice::BowVertical}})});
        // The bars do not change along y, so a vertical bow has nothing to move.
        QCOMPARE(edgeRight(bowV, 90), base);
        QVERIFY(maxDifference(bowV, plain) <= 2);
        // Square types differ from round ones.
        const QImage sq = barsWith(
            {effect(t, fxid::Dent, {{k::Amount, 80.0}, {k::Size, 150.0}, {k::CenterX, 20.0}},
                {{k::Mode, fxchoice::SquareBulge}})});
        QVERIFY(maxDifference(sq, bulge) > 20);
        // Edges: transparent shows what is below where the dent pulls from outside the picture.
        const QImage open = barsWith({effect(t, fxid::Dent, {{k::Amount, 100.0}, {k::Size, 400.0}},
            {{k::Mode, fxchoice::Pinch}, {k::Edges, fxchoice::Transparent}})});
        const QImage repeat
            = barsWith({effect(t, fxid::Dent, {{k::Amount, 100.0}, {k::Size, 400.0}},
                {{k::Mode, fxchoice::Pinch}, {k::Edges, fxchoice::Repeat}})});
        QVERIFY(maxDifference(open, repeat) > 40);
    }

    void fastNoiseEffect()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        // Normal mode at 100 %: the picture is replaced by the noise, the documented formula.
        const QImage img = barsWith({effect(t, fxid::FastNoise,
            {{k::Amount, 100.0}, {k::Scale, 100.0}}, {{k::Mode, fxchoice::Normal}})});
        for (const QPoint pt : {QPoint(10, 10), QPoint(100, 60), QPoint(250, 150)}) {
            const float n = tp::noiseGradientValue(pt.x() + 0.5f, pt.y() + 0.5f, 100.f, 4, 0.f, 1);
            const int expected = to8(std::clamp((n - 0.5f) * 2.f + 0.5f, 0.f, 1.f));
            QVERIFY2(near(px(img, pt.x(), pt.y()), expected, expected, expected, 255, 2),
                describe(px(img, pt.x(), pt.y())).constData());
        }
        // Blend 50 % overlay (default) on a grey picture: recognisable but changed (the pure
        // bar colours would not move: overlay leaves 0 and 1 alone).
        const QImage plain = barsWith({});
        const QImage greyPlain = windowWith({});
        const QImage mixed = windowWith({effect(t, fxid::FastNoise)});
        const int diff = maxDifference(greyPlain, mixed);
        QVERIFY2(diff > 20 && countDifferent(greyPlain, mixed, 4) > kW * kH / 20,
            qPrintable(QString::number(diff)));
        // Evolution and drift speed move the noise; seed changes it; still noise stays still.
        QVERIFY(
            maxDifference(mixed, windowWith({effect(t, fxid::FastNoise, {{k::Evolution, 2.5}})}))
            > 20);
        const QImage f0 = windowWith({effect(t, fxid::FastNoise, {{k::Speed, 1.0}})}, 0);
        const QImage f10 = windowWith({effect(t, fxid::FastNoise, {{k::Speed, 1.0}})}, 10);
        QVERIFY(maxDifference(f0, f10) > 20);
        QCOMPARE(maxDifference(mixed, windowWith({effect(t, fxid::FastNoise)}, 10)), 0);
        QVERIFY(
            maxDifference(mixed, windowWith({effect(t, fxid::FastNoise, {{k::Seed, 5.0}})})) > 20);
        // Screen mode never darkens, multiply never brightens.
        const QImage screen = barsWith(
            {effect(t, fxid::FastNoise, {{k::Amount, 100.0}}, {{k::Mode, fxchoice::Screen}})});
        const QImage multiply = barsWith(
            {effect(t, fxid::FastNoise, {{k::Amount, 100.0}}, {{k::Mode, fxchoice::Multiply}})});
        for (int x = 0; x < kW; x += 7) {
            QVERIFY(qGray(screen.pixel(x, 20)) >= qGray(plain.pixel(x, 20)) - 2);
            QVERIFY(qGray(multiply.pixel(x, 20)) <= qGray(plain.pixel(x, 20)) + 2);
        }
    }

    void contrastPopEffect()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        const QImage plain = windowWith({});
        const QImage pop
            = windowWith({effect(t, fxid::ContrastPop, {{k::Amount, 100.0}, {k::Radius, 30.0}})});
        // The lighter window gets lighter near its edge, the darker surround darker next to it.
        const int in0 = qGray(plain.pixel(106, 90)), in1 = qGray(pop.pixel(106, 90));
        const int out0 = qGray(plain.pixel(94, 90)), out1 = qGray(pop.pixel(94, 90));
        qInfo("contrast pop: inside %d -> %d, outside %d -> %d", in0, in1, out0, out1);
        QVERIFY(in1 > in0 + 3);
        QVERIFY(out1 < out0 - 3);
        // A negative strength flattens.
        const QImage flat
            = windowWith({effect(t, fxid::ContrastPop, {{k::Amount, -80.0}, {k::Radius, 30.0}})});
        QVERIFY(qGray(flat.pixel(106, 90)) < in0);
        // Colour is kept: red detail on a dull red does not change hue.
        const QImage red
            = windowWith({effect(t, fxid::ContrastPop, {{k::Amount, 100.0}, {k::Radius, 30.0}})}, 0,
                QColor(200, 70, 70), QColor(90, 60, 60));
        const Px c = px(red, 106, 90);
        QVERIFY(c.r > c.g + 40 && std::abs(c.g - c.b) <= 3);
        // Saturation is separate: a grey picture has none to remove.
        const QImage desat = windowWith(
            {effect(t, fxid::ContrastPop, {{k::Amount, 0.0}, {k::Saturation, -100.0}})});
        QVERIFY(maxDifference(desat, plain) <= 2);
        // ... a coloured one loses it all.
        const Px grey = px(
            windowWith({effect(t, fxid::ContrastPop, {{k::Amount, 0.0}, {k::Saturation, -100.0}})},
                0, QColor(200, 70, 70), QColor(90, 60, 60)),
            160, 90);
        QVERIFY(std::abs(grey.r - grey.g) <= 2 && std::abs(grey.g - grey.b) <= 2);
    }

    void chromaticAberrationFix()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        // A white window on black (160 x 90 px, edges at x 80 and 240): red / blue shifts put
        // colour fringes on its edges. Effects see the layer before the clip's transform.
        auto letter = [&](const QVector<EffectInstance>& effects) {
            return windowWith(effects, 0, Qt::white, Qt::black, 160, 90);
        };
        auto fringe = [](const QImage& img) {
            int worst = 0;
            for (int x = 0; x < kW; ++x)
                worst = std::max(worst, std::abs(qRed(img.pixel(x, 90)) - qBlue(img.pixel(x, 90))));
            return worst;
        };
        const QImage clean = letter({});
        QCOMPARE(fringe(clean), 0);
        const QImage shifted = letter(
            {effect(t, fxid::ChromaticAberrationFix, {{k::RedShift, 2.0}, {k::BlueShift, -2.0}})});
        QVERIFY2(fringe(shifted) > 100, qPrintable(QString::number(fringe(shifted))));
        // Red at +2 % samples farther out, so red content sits closer to the centre: at the
        // right edge red ends before blue.
        int redEnd = 0, blueEnd = 0;
        for (int x = 160; x < kW; ++x) {
            redEnd = qRed(shifted.pixel(x, 90)) > 128 ? x : redEnd;
            blueEnd = qBlue(shifted.pixel(x, 90)) > 128 ? x : blueEnd;
        }
        QVERIFY2(redEnd < blueEnd, qPrintable(QString("%1 %2").arg(redEnd).arg(blueEnd)));
        // Fix a lens that added aberration: Chromatic Aberration adds 8 px at the corners, the
        // fix with the opposite scales brings the fringes mostly back.
        const QImage broken = letter({effect(t, fxid::ChromaticAberration, {{k::Amount, 8.0}})});
        QVERIFY(fringe(broken) > 100);
        const double kShift = 8.0 / std::hypot(160.0, 90.0);
        const QImage fixed = letter({effect(t, fxid::ChromaticAberration, {{k::Amount, 8.0}}),
            effect(t, fxid::ChromaticAberrationFix,
                {{k::RedShift, 100.0 * kShift / (1.0 - kShift)},
                    {k::BlueShift, -100.0 * kShift / (1.0 + kShift)}})});
        qInfo("chromatic aberration fix: fringe %d -> %d", fringe(broken), fringe(fixed));
        QVERIFY(fringe(fixed) < fringe(broken) / 2);
        // Fringe removal pulls the colour at edges toward grey.
        const QImage defringed = letter({effect(t, fxid::ChromaticAberrationFix,
            {{k::RedShift, 2.0}, {k::BlueShift, -2.0}, {k::Defringe, 100.0}})});
        QVERIFY2(fringe(defringed) < fringe(shifted) / 2,
            qPrintable(QString("%1 %2").arg(fringe(defringed)).arg(fringe(shifted))));
        QVERIFY(
            near(px(defringed, 160, 90), 255, 255, 255, 255, 2)); // the flat inside is not touched
    }

    void falseColorBands()
    {
        // Exposure bands: black is purple, middle grey area green, near-white red; a grey in
        // an unflagged range keeps its value.
        const ParamSet p
            = EffectRegistry::instance().defaultParams(QLatin1String(fxid::FalseColor));
        const fxmath::FalseColorParams fc = fxmath::falseColorParams(p, 0);
        const fxmath::Rgb black = fxmath::falseColor(fc, {0.f, 0.f, 0.f});
        QVERIFY(black.b > black.g && black.r > black.g);
        const fxmath::Rgb mid = fxmath::falseColor(fc, {0.45f, 0.45f, 0.45f});
        QVERIFY(mid.g > mid.r && mid.g > mid.b);
        const fxmath::Rgb top = fxmath::falseColor(fc, {1.f, 1.f, 1.f});
        QVERIFY(top.r > 0.9f && top.g < 0.2f);
        const fxmath::Rgb keep = fxmath::falseColor(fc, {0.3f, 0.3f, 0.3f});
        QCOMPARE(keep.r, 0.3f);
        // Thermal is a smooth ramp: brighter input never gets a darker blue component swing,
        // and the ends are the end stops.
        ParamSet th = p;
        th.setStatic(k::Palette, fxchoice::Thermal);
        const fxmath::FalseColorParams t = fxmath::falseColorParams(th, 0);
        QCOMPARE(fxmath::falseColor(t, {0.f, 0.f, 0.f}).r, 0.f);
        QVERIFY(fxmath::falseColor(t, {1.f, 1.f, 1.f}).g > 0.95f);
        ParamSet none = p;
        none.setBase(k::Amount, 0.0);
        const fxmath::Rgb same
            = fxmath::falseColor(fxmath::falseColorParams(none, 0), {0.2f, 0.4f, 0.6f});
        QCOMPARE(same.g, 0.4f);
    }

    void dehazeEffect()
    {
        if (!gpuReady())
            QSKIP("no OpenGL");
        TimelineData t = emptyTimeline();
        // A dark square and a bright background veiled by haze (a light grey at 55 % over
        // them): Dehaze brings the contrast back.
        auto hazy = [&](const QVector<EffectInstance>& effects) {
            TimelineData tl = emptyTimeline(kW, kH);
            ClipData scene = solidClip(tl, QColor(150, 150, 150), 0, 10);
            addClip(tl, TrackType::Video, 0, scene);
            ClipData dark = solidClip(tl, QColor(30, 40, 60), 0, 10);
            dark.transform.setBase(keys::ZoomX, 0.4);
            dark.transform.setBase(keys::ZoomY, 0.5);
            addClip(tl, TrackType::Video, 1, dark);
            ClipData veil = solidClip(tl, QColor(216, 220, 226), 0, 10);
            veil.transform.setBase(keys::Opacity, 55.0);
            addClip(tl, TrackType::Video, 2, veil);
            ClipData adj;
            adj.id = tl.allocateId();
            adj.kind = ClipKind::Adjustment;
            adj.duration = 10;
            adj.effects = effects;
            addClip(tl, TrackType::Video, 3, adj);
            return render(*m_gpu, tl);
        };
        const QImage before = hazy({});
        const QImage after = hazy({effect(t, fxid::Dehaze, {{k::Amount, 90.0}, {k::Radius, 8.0}})});
        const int darkBefore = qGray(before.pixel(160, 90)),
                  darkAfter = qGray(after.pixel(160, 90));
        const int brightBefore = qGray(before.pixel(10, 10)),
                  brightAfter = qGray(after.pixel(10, 10));
        qInfo("dehaze: dark %d -> %d, bright %d -> %d", darkBefore, darkAfter, brightBefore,
            brightAfter);
        QVERIFY(darkAfter < darkBefore - 15);
        QVERIFY((brightAfter - darkAfter) > (brightBefore - darkBefore) + 4);
        QVERIFY(maxDifference(before, hazy({effect(t, fxid::Dehaze, {{k::Amount, 0.0}})})) <= 1);
    }

    // ---- Effects-section templates ----

    void templatesMakeAdjustmentClips()
    {
        const QSizeF frame(1920, 1080);
        auto make = [&](const QString& id) {
            const auto clip = effectactions::makeGeneratorClip(id, Rational {25, 1}, frame);
            return clip ? *clip : ClipData {};
        };
        const ClipData border = make(g::ColoredBorderClip);
        QCOMPARE(border.kind, ClipKind::Adjustment);
        QCOMPARE(border.effects.size(), 1);
        QCOMPARE(border.effects[0].type, QLatin1String(fxid::ColoredBorder));
        QVERIFY(border.effects[0].params.value(k::Width, 0, 0) > 10);
        const ClipData bino = make(g::BinocularsClip);
        QCOMPARE(bino.kind, ClipKind::Adjustment);
        QCOMPARE(bino.masks.size(), 3);
        QCOMPARE(bino.masks[0].mode, MaskMode::Add);
        QCOMPARE(bino.masks[1].mode, MaskMode::Subtract);
        QCOMPARE(bino.masks[1].shape, MaskShape::Ellipse);
        QVERIFY(bino.masks[1].value(maskkeys::PosX, 0) < 0
            && bino.masks[2].value(maskkeys::PosX, 0) > 0);
        const ClipData night = make(g::NightVisionClip);
        QCOMPARE(night.kind, ClipKind::Adjustment);
        QVERIFY(night.effects.size() >= 3);
        const ClipData comp = make(g::CompositionClip);
        QCOMPARE(comp.kind, ClipKind::Solid);
        QVERIFY(comp.composition.has_value());
        QCOMPARE(comp.solidColor.alpha(), 0);
        for (const QString& id :
            {g::ColoredBorderClip, g::BinocularsClip, g::NightVisionClip, g::CompositionClip})
            QCOMPARE(make(id).name, EffectRegistry::instance().find(id)->name);
    }

    void templatesRender()
    {
        const QSizeF frame(kW, kH);
        auto build = [&](const QString& id, const QColor& below) {
            TimelineData tl = emptyTimeline(kW, kH);
            addClip(tl, TrackType::Video, 0, solidClip(tl, below, 0, 100));
            ClipData tpl = *effectactions::makeGeneratorClip(id, Rational {25, 1}, frame);
            addClip(tl, TrackType::Video, 1, placed(tl, tpl));
            return tl;
        };
        if (!gpuReady())
            QSKIP("no OpenGL");
        // Colored border: the frame edge takes the border colour, the middle is untouched.
        const QImage border = render(*m_gpu, build(g::ColoredBorderClip, QColor(40, 90, 160)));
        QVERIFY(near(px(border, kW / 2, kH / 2), 40, 90, 160, 255, 1));
        QVERIFY(near(px(border, kW / 2, 2), 0xf2, 0xb1, 0x34, 255, 3));
        QVERIFY(near(px(border, 2, kH / 2), 0xf2, 0xb1, 0x34, 255, 3));
        // Binoculars: black corners and between-lens top, picture inside the lenses.
        const QImage bino = render(*m_gpu, build(g::BinocularsClip, Qt::white));
        QVERIFY2(qGray(bino.pixel(3, 3)) < 10, describe(px(bino, 3, 3)).constData());
        QVERIFY(qGray(bino.pixel(kW - 3, kH - 3)) < 10);
        QVERIFY2(qGray(bino.pixel(kW / 2, kH / 2)) > 245,
            describe(px(bino, kW / 2, kH / 2)).constData());
        QVERIFY(qGray(bino.pixel(kW / 2 - 54, kH / 2)) > 245);
        QVERIFY(qGray(bino.pixel(kW / 2 + 54, kH / 2)) > 245);
        QVERIFY(qGray(bino.pixel(kW / 2, 3)) < 10);
        // Night vision: a grey picture comes out green.
        const QImage night = render(*m_gpu, build(g::NightVisionClip, QColor(140, 140, 140)));
        const Px c = px(night, kW / 2, kH / 2);
        QVERIFY2(c.g > c.r + 30 && c.g > c.b + 20, describe(c).constData());
        // Composition clip: nothing shows, the track below does.
        const QImage comp = render(*m_gpu, build(g::CompositionClip, QColor(200, 0, 0)));
        QVERIFY(near(px(comp, kW / 2, kH / 2), 200, 0, 0, 255, 1));
        QVERIFY(near(px(render(*m_cpu, build(g::CompositionClip, QColor(200, 0, 0))), 10, 10), 200,
            0, 0, 255, 1));
    }

    // ---- save and load ----

    void projectRoundTrip()
    {
        Project project;
        TimelineData& tl = project.timeline()->mutableData();
        tl = emptyTimeline();
        for (const auto& [id, keyList] : kGenerators) {
            Q_UNUSED(keyList);
            ClipData clip = generatorClip(tl, QString::fromLatin1(id));
            clip.start = int64_t(tl.videoTracks[0].clips.size()) * 100;
            addClip(tl, TrackType::Video, 0, clip);
        }
        tl.videoTracks[0].clips[5].title.setStatic(g::Shape, QStringLiteral("ellipse"));
        tl.videoTracks[0].clips[5].title.setBase(g::Softness, 12.5);
        tl.videoTracks[0].clips[6].title.setStatic(g::Channel, QStringLiteral("cb"));
        tl.videoTracks[0].clips[7].title.setBase(g::Detail, 6);
        ClipData fx = solidClip(tl, Qt::green, 0, 50);
        for (const auto& [id, group] : kEffects)
            fx.effects.append(effect(tl, id));
        fx.effects[4].params.setStatic(k::Mode, fxchoice::SquarePinch);
        fx.effects[4].params.values[k::Amount].setKey(0, 10.0);
        fx.effects[4].params.values[k::Amount].setKey(20, 70.0);
        fx.effects[5].params.setStatic(k::Mode, fxchoice::Multiply);
        fx.effects[6].params.setStatic(k::SourceGamut, fxchoice::DciP3);
        addClip(tl, TrackType::Video, 1, fx);
        project.timeline()->notifyChanged();
        const QString path = m_dir.filePath(QStringLiteral("gap.kinoklip"));
        QString error;
        QVERIFY2(project.save(path, &error), qPrintable(error));
        Project loaded;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        const auto& gens = loaded.timeline()->data().videoTracks[0].clips;
        const auto& orig = tl.videoTracks[0].clips;
        QCOMPARE(gens.size(), orig.size());
        for (size_t i = 0; i < orig.size(); ++i) {
            QCOMPARE(gens[i].generator, orig[i].generator);
            QCOMPARE(gens[i].title.statics, orig[i].title.statics);
            for (const QString& key : orig[i].title.values.keys())
                QCOMPARE(gens[i].title.value(key, 3, -1), orig[i].title.value(key, 3, -1));
        }
        const ClipData& back = loaded.timeline()->data().videoTracks[1].clips[0];
        QCOMPARE(back.effects.size(), fx.effects.size());
        for (int i = 0; i < fx.effects.size(); ++i) {
            QCOMPARE(back.effects[i].type, fx.effects[i].type);
            QCOMPARE(back.effects[i].params.statics, fx.effects[i].params.statics);
            for (const QString& key : fx.effects[i].params.values.keys())
                QCOMPARE(back.effects[i].params.value(key, 15, -1),
                    fx.effects[i].params.value(key, 15, -1));
        }
        QVERIFY(back.effects[4].params.values.value(k::Amount).isAnimated());
        QCOMPARE(back.effects[4].params.staticValue(k::Mode).toString(), fxchoice::SquarePinch);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestEffectsGap>(argc, argv);
}

#include "test_effectsgap.moc"
