// SPDX-License-Identifier: GPL-3.0-or-later
// Colour emoji in titles and captions (spec 69): the text is split into emoji and plain runs,
// the emoji are drawn in colour (with the title's shadow), and the plain text keeps its outline.
// The drawing tests need a colour emoji font: the system's, or the file named by KK_EMOJI_FONT.
#include "core/ParamKeys.h"
#include "core/Subtitles.h"
#include "render/CaptionRenderer.h"
#include "render/EmojiText.h"
#include "render/TitleRenderer.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QTest>

using namespace kk;
using namespace kk::render;

namespace {

/// Number of hue buckets (30 degrees wide) that at least @p minPixels clearly coloured,
/// opaque pixels of @p image fall in.
int distinctHues(const QImage& image, int minPixels = 12)
{
    int buckets[12] = {};
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.alpha() < 220 || c.hsvSaturationF() < 0.35 || c.valueF() < 0.25)
                continue;
            ++buckets[std::clamp(c.hsvHue() / 30, 0, 11)];
        }
    }
    int n = 0;
    for (int count : buckets)
        n += count >= minPixels ? 1 : 0;
    return n;
}

QImage onCanvas(const TitleStyle& style)
{
    const TitleImage t = renderTitle(style);
    QImage canvas(style.canvas, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    if (!t.image.isNull()) {
        QPainter p(&canvas);
        p.drawImage(t.rect.topLeft(), t.image);
    }
    return canvas;
}

TitleStyle emojiStyle(const QString& text)
{
    TitleStyle s;
    s.text = text;
    s.size = 90;
    s.color = Qt::white;
    s.canvas = QSize(480, 270);
    return s;
}

} // namespace

class TestEmoji : public QObject
{
    Q_OBJECT
    bool m_colourFont = false;

private slots:
    void initTestCase()
    {
        emoji::registerBundledFont();
        m_colourFont = emoji::hasColourFont();
    }

    // Spec 69.1: emoji sequences are found as one run each.
    void spec69_1_splitsEmojiSequences()
    {
        using R = emoji::Run;
        auto texts = [](const QVector<R>& runs) {
            QStringList out;
            for (const R& r : runs)
                out << (r.emoji ? QStringLiteral("[") + r.text + QStringLiteral("]") : r.text);
            return out.join(QString());
        };
        QCOMPARE(texts(emoji::split(QStringLiteral("Hi \U0001F600!"))),
            QStringLiteral("Hi [\U0001F600]!"));
        // Flag (two regional indicators), family (zero width joiners), skin tone, keycap,
        // text-default symbol with and without the emoji selector.
        QCOMPARE(texts(emoji::split(QStringLiteral("\U0001F1E9\U0001F1EA"))),
            QStringLiteral("[\U0001F1E9\U0001F1EA]"));
        const QString family = QStringLiteral("\U0001F468‍\U0001F469‍\U0001F467");
        QCOMPARE(texts(emoji::split(family)), QStringLiteral("[") + family + QStringLiteral("]"));
        const QString thumb = QStringLiteral("\U0001F44D\U0001F3FD");
        QCOMPARE(texts(emoji::split(thumb + thumb)),
            QStringLiteral("[") + thumb + QStringLiteral("][") + thumb + QStringLiteral("]"));
        const QString keycap = QStringLiteral("1️⃣");
        QCOMPARE(texts(emoji::split(QStringLiteral("a") + keycap + QStringLiteral("2"))),
            QStringLiteral("a[") + keycap + QStringLiteral("]2"));
        QCOMPARE(texts(emoji::split(QStringLiteral("❤️ ❤"))), QStringLiteral("[❤️] ❤"));
        QCOMPARE(texts(emoji::split(QStringLiteral("✅ ★ ©"))), QStringLiteral("[✅] ★ ©"));
        // Runs always join back to the line.
        const QString mixed = QStringLiteral("x\U0001F680y\U0001F3F3️‍\U0001F308z");
        QString joined;
        for (const R& r : emoji::split(mixed))
            joined += r.text;
        QCOMPARE(joined, mixed);
        QVERIFY(emoji::containsEmoji(mixed));
        QVERIFY(!emoji::containsEmoji(QStringLiteral("Plain text, digits 123 äö")));
        QVERIFY(emoji::split(QString()).isEmpty());
    }

    // Spec 69.2: an emoji in a title shows in colour, not as a box or a black outline.
    void spec69_2_titleEmojiAreInColour()
    {
        if (!m_colourFont)
            QSKIP("no colour emoji font (set KK_EMOJI_FONT to a Noto Color Emoji file)");
        const QImage picture
            = onCanvas(emojiStyle(QStringLiteral("\U0001F34E\U0001F34F\U0001F30A")));
        QVERIFY2(distinctHues(picture) > 2, qPrintable(QString::number(distinctHues(picture))));
        // White plain text has no colour: a title without emoji has none either.
        QCOMPARE(distinctHues(onCanvas(emojiStyle(QStringLiteral("Apple")))), 0);
        // Mixed text: the words stay white, the emoji coloured, the block stays centred.
        TitleStyle mixed = emojiStyle(QStringLiteral("Fun \U0001F34E\U0001F30A time"));
        const QImage m = onCanvas(mixed);
        QVERIFY(distinctHues(m) >= 2);
        const TitleImage t = renderTitle(mixed);
        QVERIFY(std::abs(t.rect.center().x() - 240) < 6);
    }

    // Spec 69.3: stroke, shadow and background work around emoji.
    void spec69_3_outlineShadowAndBackground()
    {
        if (!m_colourFont)
            QSKIP("no colour emoji font (set KK_EMOJI_FONT to a Noto Color Emoji file)");
        TitleStyle s = emojiStyle(QStringLiteral("Go \U0001F34E\U0001F30A"));
        s.outlineWidth = 4;
        s.outlineColor = QColor(0, 0, 255);
        s.shadow = true;
        s.bgEnabled = true;
        s.bgColor = QColor(40, 40, 40);
        s.bgOpacity = 100;
        const QImage picture = onCanvas(s);
        QVERIFY(distinctHues(picture) >= 2);
        // The word "Go" is outlined in blue, drawn from the path.
        int blue = 0;
        for (int y = 0; y < picture.height(); ++y)
            for (int x = 0; x < picture.width(); ++x)
                blue += (qBlue(picture.pixel(x, y)) > 200 && qRed(picture.pixel(x, y)) < 60);
        QVERIFY2(blue > 100, qPrintable(QString::number(blue)));
        // Rotated text turns its emoji too.
        TitleStyle turned = emojiStyle(QStringLiteral("\U0001F34E\U0001F34F\U0001F30A"));
        turned.rotation = 90;
        const TitleImage flat = renderTitle(emojiStyle(turned.text));
        const TitleImage up = renderTitle(turned);
        QVERIFY(up.rect.height() > flat.rect.height() * 1.5);
        QVERIFY(distinctHues(onCanvas(turned)) > 2);
    }

    // Spec 69.4: the same in a subtitle (the caption track's rendering).
    void spec69_4_captionEmojiAreInColour()
    {
        if (!m_colourFont)
            QSKIP("no colour emoji font (set KK_EMOJI_FONT to a Noto Color Emoji file)");
        TrackData track = subtitles::makeSubtitleTrack();
        track.subtitleStyle.setBase(keys::Size, 60);
        const ClipData caption
            = subtitles::makeCaption(QStringLiteral("\U0001F34E\U0001F34F\U0001F30A"), 0, 25);
        CaptionPlacement placement;
        const TitleStyle style
            = resolveCaptionStyle(caption, &track, 0, QSize(640, 360), 1.0, &placement);
        const TitleImage t = renderTitle(style);
        QVERIFY(!t.image.isNull());
        QVERIFY2(distinctHues(t.image) > 2, qPrintable(QString::number(distinctHues(t.image))));
    }

    // Spec 69.5: a title layer (spec 68) with emoji draws them too.
    void spec69_5_emojiInALayer()
    {
        if (!m_colourFont)
            QSKIP("no colour emoji font (set KK_EMOJI_FONT to a Noto Color Emoji file)");
        TitleStyle layered;
        layered.canvas = QSize(480, 270);
        layered.stack.push_back(emojiStyle(QStringLiteral("\U0001F34E\U0001F34F\U0001F30A")));
        TitleStyle box;
        box.kind = TitleStyle::Kind::Shape;
        box.canvas = layered.canvas;
        box.color = QColor(30, 30, 30);
        box.posX = 0.5;
        box.posY = 0.5;
        layered.stack.insert(layered.stack.begin(), box); // under the emoji
        QVERIFY(distinctHues(onCanvas(layered)) > 2);
    }
};

QTEST_MAIN(TestEmoji)
#include "test_emoji.moc"
