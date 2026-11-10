// SPDX-License-Identifier: GPL-3.0-or-later
// Contact sheets of the 1.2 generators and effects (spec 56): every new generator, and every
// new effect on a test picture, as labelled PNG sheets for looking at. Writes into the folder
// named by KK_EFFECT_SHEETS (never into the repository) and skips when that is not set, or
// without OpenGL. Run: KK_EFFECT_SHEETS=~/cutwise-effect-sheets ./test_effectsheets
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/VideoEffectKeys.h"
#include "render/Compositor.h"
#include "ui/effects/EffectActions.h"

#include <QDir>
#include <QPainter>

using namespace kk;
using namespace kk::test;
using namespace kk::render;

namespace {

namespace g = generators;
namespace k = fxkeys;

constexpr int kTileW = 384, kTileH = 216, kCols = 3, kLabelH = 22;

struct Tile
{
    QString label;
    QImage image;
};

/// Writes @p tiles as a grid into @p path.
bool writeSheet(const QString& path, const QVector<Tile>& tiles)
{
    const int rows = (tiles.size() + kCols - 1) / kCols;
    QImage sheet(kCols * kTileW, rows * (kTileH + kLabelH), QImage::Format_RGB32);
    sheet.fill(QColor(30, 30, 34));
    QPainter p(&sheet);
    p.setPen(Qt::white);
    for (int i = 0; i < tiles.size(); ++i) {
        const int x = (i % kCols) * kTileW, y = (i / kCols) * (kTileH + kLabelH);
        p.drawImage(QRect(x, y + kLabelH, kTileW, kTileH),
            tiles[i].image.scaled(kTileW, kTileH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        p.drawText(QRect(x + 6, y, kTileW - 6, kLabelH), Qt::AlignVCenter, tiles[i].label);
    }
    return sheet.save(path);
}

ClipData generatorClip(TimelineData& tl, const QString& id)
{
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Generator;
    c.generator = id;
    c.title = EffectRegistry::instance().defaultParams(id);
    c.duration = 100;
    return c;
}

} // namespace

class TestEffectSheets : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    QString m_out;

    QImage render(const TimelineData& tl, int64_t frame = 10)
    {
        return m_gpu->renderImage(snap(tl), frame);
    }

    /// The test picture: a four colour gradient, a soft white ellipse, a dark square, and
    /// coloured text, so colour, edges, detail and flat areas are all there.
    TimelineData picture(const QVector<EffectInstance>& effects = {}, QSize size = {640, 360})
    {
        TimelineData tl = emptyTimeline(size.width(), size.height());
        ClipData base = generatorClip(tl, g::FourColorGradient);
        base.effects = effects;
        addClip(tl, TrackType::Video, 0, base);
        return tl;
    }

private slots:
    void initTestCase()
    {
        m_out = qEnvironmentVariable("KK_EFFECT_SHEETS");
        m_gpu = std::make_unique<Compositor>();
    }

    void generators()
    {
        if (m_out.isEmpty() || !m_gpu->isGpu())
            QSKIP("KK_EFFECT_SHEETS not set, or no OpenGL");
        QDir().mkpath(m_out);
        QVector<Tile> tiles;
        for (const QString& id : {g::Steps100mV, g::EbuBars, g::Bt2111HlgNarrow, g::Bt2111PqNarrow,
                 g::Bt2111PqFull, g::Window, g::YcbcrRamp, g::NoiseGradient, g::Grid}) {
            TimelineData tl = emptyTimeline(1280, 720);
            if (id == g::Grid) {
                addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(50, 90, 140), 0, 100));
                addClip(tl, TrackType::Video, 1, generatorClip(tl, id));
            } else {
                addClip(tl, TrackType::Video, 0, generatorClip(tl, id));
            }
            tiles.append({EffectRegistry::instance().find(id)->name, render(tl)});
        }
        QVERIFY(writeSheet(m_out + QStringLiteral("/generators.png"), tiles));
    }

    void effects()
    {
        if (m_out.isEmpty() || !m_gpu->isGpu())
            QSKIP("KK_EFFECT_SHEETS not set, or no OpenGL");
        QDir().mkpath(m_out);
        TimelineData t = emptyTimeline();
        auto fx = [&](const char* type,
                      std::initializer_list<std::pair<const QString, double>> values = {},
                      std::initializer_list<std::pair<const QString, QVariant>> statics = {}) {
            EffectInstance e
                = EffectRegistry::instance().createEffect(QLatin1String(type), t.allocateId());
            for (const auto& [key, v] : values)
                e.params.setBase(key, v);
            for (const auto& [key, v] : statics)
                e.params.setStatic(key, v);
            return e;
        };
        struct Row
        {
            QString label;
            QVector<EffectInstance> effects;
            bool onGrid = false; ///< the effect needs an alpha edge: put it on the Grid clip
        };
        const QVector<Row> rows = {
            {QStringLiteral("original"), {}},
            {QStringLiteral("Colored Border"),
                {fx(fxid::ColoredBorder,
                    {{k::Width, 30.0}, {k::Radius, 40.0}, {k::Feather, 6.0}})}},
            {QStringLiteral("Despill (green cast)"), {fx(fxid::Despill)}},
            {QStringLiteral("Alpha Matte Grow (on the grid lines)"),
                {fx(fxid::AlphaMatteShrinkGrow, {{k::ShrinkGrow, 4.0}, {k::EdgeBlur, 2.0}})}, true},
            {QStringLiteral("Transform"),
                {fx(fxid::Transform, {{k::OffsetX, 60.0}, {k::Scale, 70.0}, {k::Rotation, 12.0}})}},
            {QStringLiteral("Dent: round bulge"),
                {fx(fxid::Dent, {{k::Amount, 80.0}, {k::Size, 260.0}})}},
            {QStringLiteral("Dent: horizontal bow"),
                {fx(fxid::Dent, {{k::Amount, 70.0}, {k::Size, 200.0}},
                    {{k::Mode, fxchoice::BowHorizontal}})}},
            {QStringLiteral("Dent: square pinch"),
                {fx(fxid::Dent, {{k::Amount, 80.0}, {k::Size, 260.0}},
                    {{k::Mode, fxchoice::SquarePinch}})}},
            {QStringLiteral("Fast Noise (overlay)"), {fx(fxid::FastNoise)}},
            {QStringLiteral("Gamut Limiter (2020 to 709, clip)"),
                {fx(fxid::GamutLimiter, {}, {{k::Mode, fxchoice::Clip}})}},
            {QStringLiteral("Contrast Pop"), {fx(fxid::ContrastPop, {{k::Amount, 150.0}})}},
            {QStringLiteral("False Color (exposure bands)"), {fx(fxid::FalseColor)}},
            {QStringLiteral("False Color (thermal)"),
                {fx(fxid::FalseColor, {}, {{k::Palette, fxchoice::Thermal}})}},
            {QStringLiteral("Dehaze"), {fx(fxid::Dehaze, {{k::Amount, 80.0}})}},
            {QStringLiteral("Chromatic Aberration Fix"),
                {fx(fxid::ChromaticAberrationFix, {{k::RedShift, 2.0}, {k::BlueShift, -2.0}})}},
        };
        QVector<Tile> tiles;
        for (const Row& r : rows) {
            // A picture with edges: the gradient with grid lines and a soft ellipse composited
            // into it by a nested look: the effects sit on the gradient clip only, so apply
            // them to a clip that already holds the details (an adjustment clip would do too).
            TimelineData tl = emptyTimeline(640, 360);
            addClip(
                tl, TrackType::Video, 0, generatorClip(tl, QStringLiteral("four_color_gradient")));
            ClipData grid = generatorClip(tl, g::Grid);
            grid.title.setBase(g::LineWidth, 3);
            grid.title.setBase(g::MajorEvery, 0);
            if (r.onGrid)
                grid.effects = r.effects;
            addClip(tl, TrackType::Video, 1, grid);
            ClipData adj;
            adj.id = tl.allocateId();
            adj.kind = ClipKind::Adjustment;
            adj.duration = 100;
            if (!r.onGrid)
                adj.effects = r.effects;
            addClip(tl, TrackType::Video, 2, adj);
            tiles.append({r.label, render(tl)});
        }
        QVERIFY(writeSheet(m_out + QStringLiteral("/effects.png"), tiles));

        // Templates over the same picture.
        QVector<Tile> templates;
        for (const QString& id : {g::ColoredBorderClip, g::BinocularsClip, g::NightVisionClip}) {
            TimelineData tl = picture({}, {640, 360});
            ClipData tpl
                = *effectactions::makeGeneratorClip(id, Rational {25, 1}, QSizeF(640, 360));
            tpl.id = tl.allocateId();
            for (EffectInstance& e : tpl.effects)
                e.id = tl.allocateId();
            for (MaskData& m : tpl.masks)
                m.id = tl.allocateId();
            tpl.duration = 100;
            addClip(tl, TrackType::Video, 1, tpl);
            templates.append({EffectRegistry::instance().find(id)->name, render(tl)});
        }
        QVERIFY(writeSheet(m_out + QStringLiteral("/templates.png"), templates));
    }

    /// The second part of the 1.2 effects (spec 57) on the same picture: sheet effects-b.png.
    void effectsB()
    {
        if (m_out.isEmpty() || !m_gpu->isGpu())
            QSKIP("KK_EFFECT_SHEETS not set, or no OpenGL");
        QDir().mkpath(m_out);
        TimelineData t = emptyTimeline();
        auto fx = [&](const char* type,
                      std::initializer_list<std::pair<const QString, double>> values = {},
                      std::initializer_list<std::pair<const QString, QVariant>> statics = {}) {
            EffectInstance e
                = EffectRegistry::instance().createEffect(QLatin1String(type), t.allocateId());
            for (const auto& [key, v] : values)
                e.params.setBase(key, v);
            for (const auto& [key, v] : statics)
                e.params.setStatic(key, v);
            return e;
        };
        enum Extra { Plain, Lights, Defects, NoiseOnly, KeyWindow };
        struct Row
        {
            QString label;
            QVector<EffectInstance> effects;
            Extra extra = Plain;
        };
        const QVector<Row> rows = {
            {QStringLiteral("original"), {}},
            {QStringLiteral("Sharpen Edges"), {fx(fxid::SharpenEdges, {{k::Amount, 300.0}})}},
            {QStringLiteral("Soften and Sharpen"),
                {fx(fxid::SoftenSharpen, {{k::Soften, 100.0}, {k::Sharpen, 150.0}})}},
            {QStringLiteral("Texture Pop (on noise)"),
                {fx(fxid::TexturePop, {{k::Amount, 200.0}})}, NoiseOnly},
            {QStringLiteral("Detail Recovery"),
                {fx(fxid::DetailRecovery, {{k::Shadows, 80.0}, {k::Highlights, 80.0}, {k::Detail, 100.0}})}},
            {QStringLiteral("Deband"), {fx(fxid::Deband)}},
            {QStringLiteral("Dead Pixel Fixer (blobs of 3 px)"),
                {fx(fxid::DeadPixelFixer, {{k::Size, 3.0}})}, Defects},
            {QStringLiteral("Beauty (on noise)"), {fx(fxid::Beauty, {{k::Smoothing, 100.0}})}, NoiseOnly},
            {QStringLiteral("Analog Damage"), {fx(fxid::AnalogDamage)}},
            {QStringLiteral("Film Damage"), {fx(fxid::FilmDamage, {{k::Dirt, 60.0}, {k::Scratches, 60.0}})}},
            {QStringLiteral("JPEG Damage"), {fx(fxid::JpegDamage, {{k::Quality, 8.0}})}},
            {QStringLiteral("Watercolor"), {fx(fxid::Watercolor)}},
            {QStringLiteral("Stylize: relief"), {fx(fxid::Stylize)}},
            {QStringLiteral("Stylize: ink outline"),
                {fx(fxid::Stylize, {}, {{k::Style, fxchoice::Ink}})}},
            {QStringLiteral("Stylize: cutout"),
                {fx(fxid::Stylize, {}, {{k::Style, fxchoice::Cutout}})}},
            {QStringLiteral("Aperture Diffraction"),
                {fx(fxid::ApertureDiffraction, {{k::Length, 160.0}})}, Lights},
            {QStringLiteral("Lens Reflections"), {fx(fxid::LensReflections)}, Lights},
            {QStringLiteral("Color Palette"), {fx(fxid::ColorPalette)}},
            {QStringLiteral("3D Keyer (green window)"), {fx(fxid::Keyer3d)}, KeyWindow},
            {QStringLiteral("Video Collage"),
                {fx(fxid::VideoCollage, {{k::Spacing, 14.0}, {k::Radius, 12.0}})}},
        };
        auto window = [&](TimelineData& tl, double w, double h, double cx, double cy,
                          const QColor& inside, const QColor& outside, const QString& shape) {
            ClipData win = generatorClip(tl, g::Window);
            win.title.setBase(g::Width, w);
            win.title.setBase(g::Height, h);
            win.title.setBase(g::CenterX, cx);
            win.title.setBase(g::CenterY, cy);
            win.title.setStatic(g::Shape, shape);
            win.title.setStatic(g::Color1, inside);
            win.title.setStatic(g::Color2, outside);
            return win;
        };
        QVector<Tile> tiles;
        for (const Row& r : rows) {
            TimelineData tl = emptyTimeline(640, 360);
            int track = 0;
            if (r.extra == NoiseOnly) {
                ClipData noise = generatorClip(tl, g::Noise);
                noise.title.setBase(g::Amount, 18);
                noise.title.setBase(g::Grain, 1);
                noise.title.setStatic(g::Monochrome, true);
                addClip(tl, TrackType::Video, track++, noise);
            } else {
                addClip(tl, TrackType::Video, track++,
                    generatorClip(tl, QStringLiteral("four_color_gradient")));
                if (r.extra != Lights) { // bright grid lines would be lights too
                    ClipData grid = generatorClip(tl, g::Grid);
                    grid.title.setBase(g::LineWidth, 3);
                    grid.title.setBase(g::MajorEvery, 0);
                    addClip(tl, TrackType::Video, track++, grid);
                }
            }
            if (r.extra == Lights) {
                const QColor none(0, 0, 0, 0);
                for (const QPointF& p : {QPointF(-150, -70), QPointF(130, 40), QPointF(40, -100)})
                    addClip(tl, TrackType::Video, track++,
                        window(tl, 16, 16, p.x(), p.y(), Qt::white, none, QStringLiteral("ellipse")));
            }
            if (r.extra == Defects) {
                const QColor none(0, 0, 0, 0);
                int n = 0;
                for (const QPointF& p : {QPointF(-200, -90), QPointF(-60, 70), QPointF(90, -40),
                         QPointF(220, 100), QPointF(10, 20)})
                    addClip(tl, TrackType::Video, track++,
                        window(tl, 3, 3, p.x(), p.y(), (n++ % 2) ? Qt::black : Qt::white, none,
                            QStringLiteral("rectangle")));
            }
            ClipData adj;
            adj.id = tl.allocateId();
            adj.kind = ClipKind::Adjustment;
            adj.duration = 100;
            if (r.extra == KeyWindow) {
                ClipData win = window(tl, 300, 200, 0, 0, QColor(0, 177, 64), QColor(230, 120, 30),
                    QStringLiteral("ellipse"));
                win.effects = r.effects;
                addClip(tl, TrackType::Video, track++, win);
            } else {
                adj.effects = r.effects;
                addClip(tl, TrackType::Video, track++, adj);
            }
            tiles.append({r.label, render(tl)});
        }
        QVERIFY(writeSheet(m_out + QStringLiteral("/effects-b.png"), tiles));
    }

    /// The template looks (spec 65) as the user gets them (Effects > Effects clips over the
    /// test picture): before on the left, after on the right, at frame 26 of the clip.
    void looks()
    {
        if (m_out.isEmpty() || !m_gpu->isGpu())
            QSKIP("KK_EFFECT_SHEETS not set, or no OpenGL");
        QDir().mkpath(m_out);
        const QString ids[] = {g::CctvClip, g::DigitalGlitchClip, g::DroneOverlayClip, g::DslrClip,
            g::DveClip, g::VideoCallClip, g::VideoCameraClip};
        constexpr int w = 640, h = 360, gap = 8;
        QImage sheet(2 * w + 3 * gap, 7 * (h + 24) + gap, QImage::Format_RGB32);
        sheet.fill(QColor(30, 30, 34));
        QPainter paint(&sheet);
        paint.setPen(Qt::white);
        int row = 0;
        for (const QString& id : ids) {
            for (int after = 0; after < 2; ++after) {
                TimelineData tl = picture({}, {w, h});
                ClipData grid = generatorClip(tl, g::Grid);
                grid.title.setBase(g::LineWidth, 2);
                grid.title.setBase(g::MajorEvery, 0);
                addClip(tl, TrackType::Video, 1, grid);
                if (after) {
                    ClipData tpl
                        = *effectactions::makeGeneratorClip(id, Rational {25, 1}, QSizeF(w, h));
                    tpl.id = tl.allocateId();
                    for (EffectInstance& e : tpl.effects)
                        e.id = tl.allocateId();
                    tpl.duration = 100;
                    addClip(tl, TrackType::Video, 2, tpl);
                }
                const int x = gap + after * (w + gap), y = row * (h + 24) + 24;
                paint.drawImage(x, y, render(tl, 26));
                if (!after)
                    paint.drawText(x, y - 7, EffectRegistry::instance().find(id)->name);
            }
            ++row;
        }
        paint.end();
        QVERIFY(sheet.save(m_out + QStringLiteral("/looks.png")));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestEffectSheets>(argc, argv);
}

#include "test_effectsheets.moc"
