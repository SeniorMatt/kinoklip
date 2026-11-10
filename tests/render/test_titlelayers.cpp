// SPDX-License-Identifier: GPL-3.0-or-later
// Title layers (spec 68): the old single-text title renders as before, layers stack bottom to
// top, keyframes and every layer operation work per layer.
#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"
#include "core/Serialization.h"
#include "core/TitleLayers.h"
#include "core/TitlePresets.h"
#include "render/TitleRenderer.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTest>

using namespace kk;
using namespace kk::render;

namespace {

/// The title the golden picture was made from (main's renderer, before layers existed).
ClipData goldenClip()
{
    ClipData c;
    c.id = 1;
    c.kind = ClipKind::Title;
    c.start = 0;
    c.duration = 50;
    c.title.setStatic(keys::Text, QStringLiteral("Golden Title\nsecond line"));
    c.title.setStatic(keys::Bold, true);
    c.title.setStatic(keys::Align, QStringLiteral("left"));
    c.title.setStatic(keys::Anchor, QStringLiteral("left"));
    c.title.setStatic(keys::Shadow, true);
    c.title.setStatic(keys::BgEnabled, true);
    c.title.setStatic(keys::Color, QColor(255, 220, 80));
    c.title.setStatic(keys::OutlineColor, QColor(20, 40, 160));
    c.title.setBase(keys::Size, 54);
    c.title.setBase(keys::Tracking, 10);
    c.title.setBase(keys::OutlineWidth, 3);
    c.title.setBase(keys::TextPosX, 0.1);
    c.title.setBase(keys::TextPosY, 0.4);
    return c;
}

QString goldenPath()
{
    return QDir(QFileInfo(QString::fromLatin1(__FILE__)).absolutePath())
        .filePath(QStringLiteral("golden/title_single_text.png"));
}

constexpr QSize kCanvas(320, 180);

/// The whole title picture at clip-local @p frame, on a canvas-sized transparent image.
QImage titlePicture(const ClipData& clip, int64_t frame = 0)
{
    const TitleImage t = renderTitle(resolveTitleStyle(clip, frame, kCanvas, 1.0));
    QImage canvas(kCanvas, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    if (!t.image.isNull()) {
        QPainter p(&canvas);
        p.drawImage(t.rect.topLeft(), t.image);
    }
    return canvas;
}

/// Bounding box of the pixels that are not transparent.
QRect inkBox(const QImage& image)
{
    QRect box;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 8)
                box = box.united(QRect(x, y, 1, 1));
        }
    }
    return box;
}

int countNear(const QImage& image, const QRect& area, const QColor& colour, int tolerance = 24)
{
    int n = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.alpha() > 200 && std::abs(c.red() - colour.red()) <= tolerance
                && std::abs(c.green() - colour.green()) <= tolerance
                && std::abs(c.blue() - colour.blue()) <= tolerance)
                ++n;
        }
    }
    return n;
}

ClipData plainTitle(const QString& text = QStringLiteral("MMMM"), double size = 80)
{
    ClipData c;
    c.id = 7;
    c.kind = ClipKind::Title;
    c.duration = 40;
    c.title.setStatic(keys::Text, text);
    c.title.setBase(keys::Size, size);
    return c;
}

/// A centred red rectangle layer, 200 x 120 px.
quint64 addRedBox(ClipData& clip)
{
    const quint64 id = titlelayers::addLayer(clip, TitleLayerKind::Shape, QStringLiteral("rect"));
    ParamSet& p = *titlelayers::params(clip, id);
    p.setStatic(keys::Color, QColor(255, 0, 0));
    p.setBase(keys::ShapeWidth, 200);
    p.setBase(keys::ShapeHeight, 120);
    return id;
}

} // namespace

class TitleLayersTest : public QObject
{
    Q_OBJECT
private slots:
    void goldenSingleText()
    {
        const TitleStyle style = resolveTitleStyle(goldenClip(), 10, QSize(640, 360), 1.0);
        const TitleImage rendered = renderTitle(style);
        QVERIFY(!rendered.image.isNull());
        // Full canvas picture, so the position is compared too.
        QImage canvas(640, 360, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        {
            QPainter p(&canvas);
            p.drawImage(rendered.rect.topLeft(), rendered.image);
        }
        if (qEnvironmentVariableIsSet("KK_WRITE_GOLDEN")) {
            QVERIFY(canvas.save(goldenPath()));
            return;
        }
        QImage golden(goldenPath());
        QVERIFY2(!golden.isNull(), qPrintable(goldenPath()));
        QCOMPARE(canvas, golden.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }

    // Spec 68.1: a title without layers is one plain text layer, drawn by the old code path.
    void spec68_1_oldTitleIsOneLayer()
    {
        const ClipData clip = goldenClip();
        QVERIFY(titlelayers::isPlain(clip));
        QCOMPARE(titlelayers::order(clip), (QVector<quint64> {titlelayers::kBaseId}));
        QVERIFY(resolveTitleStyle(clip, 10, QSize(640, 360), 1.0).stack.empty());
        // A project file of before layers (no layers keys) loads as that one layer and
        // renders the golden picture.
        QJsonObject json = serialization::clipToJson(clip);
        QVERIFY(!json.contains(QStringLiteral("layers")));
        QVERIFY(!json.contains(QStringLiteral("baseLayer")));
        const ClipData loaded = serialization::clipFromJson(json);
        QVERIFY(titlelayers::isPlain(loaded));
        QCOMPARE(loaded, clip);
        QCOMPARE(titlePicture(loaded), titlePicture(clip));
    }

    // Every title preset of the Effects library is a plain one-layer title that survives a
    // save and load unchanged.
    void spec68_2_presetsRoundTripAsOneLayer()
    {
        const EffectRegistry& registry = EffectRegistry::instance();
        int presets = 0;
        for (const EffectDescriptor* d : registry.byCategory(EffectCategory::Title)) {
            ClipData clip = plainTitle();
            clip.generator = d->id;
            clip.title = registry.defaultParams(d->id);
            const ClipData loaded = serialization::clipFromJson(serialization::clipToJson(clip));
            QVERIFY2(titlelayers::isPlain(loaded), qPrintable(d->id));
            QCOMPARE(loaded, clip);
            QCOMPARE(titlePicture(loaded), titlePicture(clip));
            ++presets;
        }
        QVERIFY(presets >= 6);
    }

    // Spec 68.3: layers are drawn bottom to top.
    void spec68_3_layersDrawBottomToTop()
    {
        ClipData clip = plainTitle();
        clip.title.setStatic(keys::Color, QColor(255, 255, 255));
        const quint64 box = addRedBox(clip);
        const QRect area(60, 30, 200, 120);
        // Text at the bottom, the red box over it: no white inside the box.
        QImage picture = titlePicture(clip);
        QVERIFY(countNear(picture, area.adjusted(4, 4, -4, -4), QColor(255, 0, 0)) > 20000);
        QCOMPARE(countNear(picture, area.adjusted(4, 4, -4, -4), QColor(255, 255, 255)), 0);
        // The box moved below the text: the text shows over it.
        QVERIFY(titlelayers::moveLayer(clip, box, -1));
        picture = titlePicture(clip);
        QVERIFY(countNear(picture, area.adjusted(4, 4, -4, -4), QColor(255, 255, 255)) > 200);
        QVERIFY(countNear(picture, area.adjusted(4, 4, -4, -4), QColor(255, 0, 0)) > 5000);
    }

    // Two text layers and a shape, in order, each with its own look.
    void spec68_4_twoTextLayersAndAShape()
    {
        ClipData clip = plainTitle(QStringLiteral("LOW"), 40);
        clip.title.setBase(keys::TextPosY, 0.2);
        clip.title.setStatic(keys::Color, QColor(0, 255, 0));
        const quint64 shape
            = titlelayers::addLayer(clip, TitleLayerKind::Shape, QStringLiteral("ellipse"));
        titlelayers::params(clip, shape)->setStatic(keys::Color, QColor(0, 0, 255));
        titlelayers::params(clip, shape)->setBase(keys::ShapeWidth, 120);
        titlelayers::params(clip, shape)->setBase(keys::ShapeHeight, 100);
        const quint64 top = titlelayers::addLayer(clip, TitleLayerKind::Text);
        ParamSet& t = *titlelayers::params(clip, top);
        t.setStatic(keys::Text, QStringLiteral("HIGH"));
        t.setStatic(keys::Color, QColor(255, 255, 0));
        t.setBase(keys::Size, 40);
        t.setBase(keys::TextPosY, 0.8);
        QCOMPARE(titlelayers::order(clip), (QVector<quint64> {0, shape, top}));
        const QImage picture = titlePicture(clip);
        QVERIFY(countNear(picture, QRect(0, 0, 320, 90), QColor(255, 255, 0)) > 100); // HIGH, top
        QVERIFY(countNear(picture, QRect(0, 90, 320, 90), QColor(0, 255, 0)) > 100); // LOW, bottom
        QVERIFY(countNear(picture, QRect(0, 0, 320, 180), QColor(0, 0, 255)) > 5000); // ellipse
        // The style of the title is the three shown layers.
        QCOMPARE(resolveTitleStyle(clip, 0, kCanvas, 1.0).stack.size(), size_t(3));
        // Hiding the shape leaves the two texts; hiding the first layer leaves one.
        titlelayers::setVisible(clip, shape, false);
        QCOMPARE(countNear(titlePicture(clip), QRect(0, 0, 320, 180), QColor(0, 0, 255)), 0);
        titlelayers::setVisible(clip, titlelayers::kBaseId, false);
        QCOMPARE(countNear(titlePicture(clip), QRect(0, 90, 320, 90), QColor(0, 255, 0)), 0);
        QVERIFY(countNear(titlePicture(clip), QRect(0, 0, 320, 90), QColor(255, 255, 0)) > 100);
        // Nothing shown draws nothing.
        titlelayers::setVisible(clip, top, false);
        QVERIFY(titlePicture(clip) == QImage(kCanvas, QImage::Format_ARGB32_Premultiplied)
            || inkBox(titlePicture(clip)).isEmpty());
    }

    // Spec 68.5: every layer has its own keyframes.
    void spec68_5_keyframesPerLayer()
    {
        ClipData clip = plainTitle(QStringLiteral("."), 10);
        const quint64 box
            = titlelayers::addLayer(clip, TitleLayerKind::Shape, QStringLiteral("rect"));
        titlelayers::setVisible(clip, titlelayers::kBaseId, false);
        ParamSet& p = *titlelayers::params(clip, box);
        p.setBase(keys::ShapeWidth, 40);
        p.setBase(keys::ShapeHeight, 40);
        p.animated(keys::TextPosX, 0.5).setKey(0, 0.25);
        p.animated(keys::TextPosX, 0.5).setKey(20, 0.75);
        const QRect at0 = inkBox(titlePicture(clip, 0));
        const QRect at20 = inkBox(titlePicture(clip, 20));
        QVERIFY2(
            std::abs(at0.center().x() - 80) <= 2, qPrintable(QString::number(at0.center().x())));
        QVERIFY2(
            std::abs(at20.center().x() - 240) <= 2, qPrintable(QString::number(at20.center().x())));
        // A second shape keeps its own place.
        const quint64 other
            = titlelayers::addLayer(clip, TitleLayerKind::Shape, QStringLiteral("rect"));
        QCOMPARE(titlelayers::params(clip, other)->value(keys::TextPosX, 10), 0.5);
        // The keys of the layer move with a head trim.
        titlelayers::shiftKeys(clip, -10);
        QCOMPARE(titlelayers::params(clip, box)->value(keys::TextPosX, 0), 0.5);
    }

    // Spec 68.6: rotation and opacity of a layer.
    void spec68_6_rotationAndOpacity()
    {
        ClipData clip = plainTitle(QStringLiteral("."), 10);
        const quint64 line
            = titlelayers::addLayer(clip, TitleLayerKind::Shape, QStringLiteral("line"));
        ParamSet& p = *titlelayers::params(clip, line);
        p.setBase(keys::ShapeWidth, 160);
        p.setBase(keys::ShapeHeight, 10);
        const QRect flat = inkBox(titlePicture(clip));
        QVERIFY(flat.width() > 150 && flat.height() < 20);
        p.setBase(keys::LayerRotation, 90);
        const QRect upright = inkBox(titlePicture(clip));
        QVERIFY(upright.height() > 150 && upright.width() < 20);
        p.setBase(keys::LayerRotation, 0);
        p.setBase(keys::LayerOpacity, 50);
        const QColor half = titlePicture(clip).pixelColor(160, 90);
        QVERIFY2(std::abs(half.alpha() - 128) <= 3, qPrintable(QString::number(half.alpha())));
        // The text of a layer turns too.
        ClipData text = plainTitle(QStringLiteral("WIDE TEXT"), 40);
        const QRect before = inkBox(titlePicture(text));
        text.title.setBase(keys::LayerRotation, 90);
        const QRect turned = inkBox(titlePicture(text));
        QVERIFY(turned.height() > turned.width());
        QVERIFY(before.width() > before.height());
    }

    // Spec 68.7: the layer operations.
    void spec68_7_layerOperations()
    {
        ClipData clip = plainTitle();
        const quint64 a = titlelayers::addLayer(clip, TitleLayerKind::Text);
        const quint64 b = titlelayers::addLayer(clip, TitleLayerKind::Shape);
        QCOMPARE(titlelayers::order(clip), (QVector<quint64> {0, a, b}));
        // The first layer moves like the others and is never deleted.
        QVERIFY(titlelayers::moveLayer(clip, titlelayers::kBaseId, +1));
        QCOMPARE(titlelayers::order(clip), (QVector<quint64> {a, 0, b}));
        QVERIFY(titlelayers::moveLayer(clip, titlelayers::kBaseId, +5));
        QCOMPARE(titlelayers::order(clip), (QVector<quint64> {a, b, 0}));
        QVERIFY(!titlelayers::moveLayer(clip, titlelayers::kBaseId, +1));
        QVERIFY(!titlelayers::removeLayer(clip, titlelayers::kBaseId));
        // Duplicate: the copy sits directly above the original with its own id.
        const quint64 copy = titlelayers::duplicateLayer(clip, a);
        QVERIFY(copy != 0 && copy != a && copy != b);
        QCOMPARE(titlelayers::order(clip), (QVector<quint64> {a, copy, b, 0}));
        QCOMPARE(*titlelayers::params(clip, copy), *titlelayers::params(clip, a));
        const quint64 baseCopy = titlelayers::duplicateLayer(clip, titlelayers::kBaseId);
        QCOMPARE(titlelayers::kind(clip, baseCopy), TitleLayerKind::Text);
        QCOMPARE(*titlelayers::params(clip, baseCopy), clip.title);
        QVERIFY(titlelayers::removeLayer(clip, baseCopy));
        // Rename and visibility.
        QVERIFY(titlelayers::rename(clip, b, QStringLiteral("Panel")));
        QCOMPARE(titlelayers::displayName(clip, b), QStringLiteral("Panel"));
        QCOMPARE(titlelayers::displayName(clip, titlelayers::kBaseId), QStringLiteral("MMMM"));
        QVERIFY(titlelayers::setVisible(clip, b, false));
        QVERIFY(!titlelayers::visible(clip, b));
        QVERIFY(titlelayers::removeLayer(clip, a));
        QCOMPARE(titlelayers::order(clip), (QVector<quint64> {copy, b, 0}));
        QVERIFY(!titlelayers::exists(clip, a));
    }

    // Spec 68.8: layers survive a save and load, with the first layer's place and name.
    void spec68_8_projectRoundTrip()
    {
        ClipData clip = plainTitle(QStringLiteral("Base"));
        const quint64 box = addRedBox(clip);
        const quint64 text = titlelayers::addLayer(clip, TitleLayerKind::Text);
        titlelayers::params(clip, text)->animated(keys::Size, 72).setKey(5, 99);
        titlelayers::rename(clip, box, QStringLiteral("Red panel"));
        titlelayers::setVisible(clip, text, false);
        titlelayers::moveLayer(clip, titlelayers::kBaseId, +1);
        titlelayers::rename(clip, titlelayers::kBaseId, QStringLiteral("Main"));
        const QJsonObject json = serialization::clipToJson(clip);
        QVERIFY(json.contains(QStringLiteral("layers")));
        const ClipData loaded = serialization::clipFromJson(json);
        QCOMPARE(loaded, clip);
        QCOMPARE(titlelayers::order(loaded), titlelayers::order(clip));
        QVERIFY(!titlelayers::visible(loaded, text));
        QCOMPARE(titlelayers::displayName(loaded, box), QStringLiteral("Red panel"));
        QCOMPARE(titlelayers::displayName(loaded, titlelayers::kBaseId), QStringLiteral("Main"));
        QCOMPARE(titlePicture(loaded), titlePicture(clip));
    }

    // Spec 68.9: the texture cache key follows the layer data.
    void spec68_9_cacheKeyFollowsLayers()
    {
        ClipData clip = plainTitle();
        const quint64 box = addRedBox(clip);
        const TitleStyle one = resolveTitleStyle(clip, 0, kCanvas, 1.0);
        QCOMPARE(one, resolveTitleStyle(clip, 0, kCanvas, 1.0));
        QCOMPARE(qHash(one), qHash(resolveTitleStyle(clip, 0, kCanvas, 1.0)));
        titlelayers::params(clip, box)->setStatic(keys::Color, QColor(0, 255, 0));
        const TitleStyle two = resolveTitleStyle(clip, 0, kCanvas, 1.0);
        QVERIFY(one != two);
        QVERIFY(qHash(one) != qHash(two));
        titlelayers::moveLayer(clip, box, -1);
        QVERIFY(two != resolveTitleStyle(clip, 0, kCanvas, 1.0));
        titlelayers::setVisible(clip, box, false);
        QCOMPARE(resolveTitleStyle(clip, 0, kCanvas, 1.0).stack.size(), size_t(1));
    }
};

QTEST_MAIN(TitleLayersTest)
#include "test_titlelayers.moc"
