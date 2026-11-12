// SPDX-License-Identifier: GPL-3.0-or-later
// Write On (Text+'s range of shown characters, spec section on titles): the pure visibility
// function, the rendered title (hidden characters keep their place), keyframes and the project
// format.
#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"
#include "core/Serialization.h"
#include "core/TitlePresets.h"
#include "render/TitleRenderer.h"
#include "ui/effects/EffectActions.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTest>

using namespace kk;
using namespace kk::render;

namespace {

TitleStyle style(const QString& text, double start = 0, double end = 1)
{
    TitleStyle s;
    s.text = text;
    s.size = 90;
    s.color = Qt::white;
    s.canvas = QSize(640, 270);
    s.writeOnStart = start;
    s.writeOnEnd = end;
    return s;
}

QImage onCanvas(const TitleStyle& s)
{
    const TitleImage t = renderTitle(s);
    QImage canvas(s.canvas, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    if (!t.image.isNull()) {
        QPainter p(&canvas);
        p.drawImage(t.rect.topLeft(), t.image);
    }
    return canvas;
}

/// Horizontal ink extent {left, right} (alpha > 128), or {-1, -1} when empty.
QPair<int, int> inkSpan(const QImage& image)
{
    int left = -1;
    int right = -1;
    for (int x = 0; x < image.width(); ++x) {
        for (int y = 0; y < image.height(); ++y) {
            if (qAlpha(image.pixel(x, y)) > 128) {
                if (left < 0)
                    left = x;
                right = x;
                break;
            }
        }
    }
    return {left, right};
}

qint64 ink(const QImage& image)
{
    qint64 sum = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            sum += qAlpha(image.pixel(x, y));
    return sum;
}

QString hidden(const QString& text, double start, double end)
{
    const QBitArray v = writeOnVisible(text, start, end);
    QString out;
    for (int i = 0; i < text.size(); ++i)
        out += v.testBit(i) ? text[i] : QLatin1Char('_');
    return out;
}

} // namespace

class TestWriteOn : public QObject
{
    Q_OBJECT
private slots:
    void visibility()
    {
        QCOMPARE(hidden("Hello", 0, 1), QString("Hello"));
        QCOMPARE(hidden("Hello", 0, 0), QString("_____"));
        QCOMPARE(hidden("Hello", 0, 0.2), QString("H____"));
        QCOMPARE(hidden("Hello", 0, 0.6), QString("Hel__"));
        QCOMPARE(hidden("Hello", 0.4, 1), QString("__llo"));
        QCOMPARE(hidden("Hello", 0.4, 0.6), QString("__l__"));
        QCOMPARE(hidden("Hello", 0.8, 0.2), QString("_____")); // empty range
        QCOMPARE(hidden("Hello", -1, 3), QString("Hello")); // clamped
    }

    void spacesCountLineBreaksDoNot()
    {
        // 5 clusters: a, space, b, c, d (the line break is not one)
        QCOMPARE(hidden("a b\ncd", 0, 0.4), QString("a _\n__"));
        QCOMPARE(hidden("a b\ncd", 0, 0.8), QString("a b\nc_"));
    }

    void graphemeClusters()
    {
        // e + combining acute is one character; the family emoji (ZWJ sequence) is one too.
        const QString text = QString::fromUtf8("e\xcc\x81x");
        QCOMPARE(writeOnVisible(text, 0, 0.4).count(true), 2); // e + mark
        const QString family = QString::fromUtf8("a\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
                                                 "\xe2\x80\x8d\xf0\x9f\x91\xa7"
                                                 "b");
        const QBitArray v = writeOnVisible(family, 0, 0.4);
        QCOMPARE(v.count(true), 1); // only "a": the family counts as one of three
        QCOMPARE(writeOnVisible(family, 0, 2.0 / 3.0).count(false), 1); // only "b" hidden
    }

    void layoutDoesNotJump()
    {
        const QImage full = onCanvas(style("HELLO WORLD"));
        const QImage half = onCanvas(style("HELLO WORLD", 0, 0.5));
        if (const QByteArray png = qgetenv("KK_WRITEON_PNG"); !png.isEmpty()) {
            QImage sheet(full.width(), full.height() * 2, QImage::Format_ARGB32_Premultiplied);
            sheet.fill(QColor(40, 40, 48));
            QPainter p(&sheet);
            p.drawImage(0, 0, half);
            p.drawImage(0, full.height(), full);
            p.end();
            sheet.save(QString::fromLocal8Bit(png));
        }
        const QPair<int, int> f = inkSpan(full);
        const QPair<int, int> h = inkSpan(half);
        QVERIFY(h.first >= 0);
        QVERIFY(qAbs(h.first - f.first) <= 2); // first letter stays where it will be
        QVERIFY(h.second < f.second - 60); // the rest is not drawn
        QVERIFY(ink(half) < ink(full));
        // Erasing from the start leaves the end of the line in place.
        const QImage tail = onCanvas(style("HELLO WORLD", 0.5, 1));
        QVERIFY(qAbs(inkSpan(tail).second - f.second) <= 2);
        QVERIFY(inkSpan(tail).first > f.first + 60);
    }

    void centredLinesDoNotShift()
    {
        const QImage full = onCanvas(style("ab\ncdefghij"));
        const QImage first = onCanvas(style("ab\ncdefghij", 0, 0.2)); // "ab"
        const QPair<int, int> f = inkSpan(full);
        const QPair<int, int> a = inkSpan(first);
        // The short centred line "ab" is not re-centred while the long one is hidden.
        QVERIFY(a.first > f.first + 40);
    }

    void nothingTypedYetIsEmpty() { QVERIFY(renderTitle(style("Hello", 0, 0)).image.isNull()); }

    void defaultIsUnchanged()
    {
        ParamSet p;
        ClipData clip;
        const TitleStyle s
            = resolveLayerStyle(p, TitleLayerKind::Text, clip, 0, QSize(640, 270), 1.0);
        QCOMPARE(s.writeOnStart, 0.0);
        QCOMPARE(s.writeOnEnd, 1.0);
        TitleStyle a = style("Hello");
        TitleStyle b = a;
        b.writeOnStart = 0;
        b.writeOnEnd = 1;
        QCOMPARE(onCanvas(a), onCanvas(b));
    }

    void keyframedEndAnimates()
    {
        ParamSet p;
        p.setStatic(keys::Text, QStringLiteral("Typewriter"));
        p.setBase(keys::Size, 90);
        AnimatedValue& end = p.animated(keys::WriteOnEnd, 1.0);
        end.setKey(0, 0.0);
        end.setKey(24, 1.0);
        ClipData clip;
        qint64 last = -1;
        for (int frame : {0, 6, 12, 18, 24}) {
            const TitleStyle s
                = resolveLayerStyle(p, TitleLayerKind::Text, clip, frame, QSize(640, 270), 1.0);
            const qint64 now = ink(onCanvas(s));
            QVERIFY2(now >= last, qPrintable(QString::number(frame)));
            if (frame < 24)
                QVERIFY(now < ink(onCanvas(resolveLayerStyle(
                            p, TitleLayerKind::Text, clip, 24, QSize(640, 270), 1.0))));
            last = now;
        }
        QVERIFY(last > 0);
        // The writing is cached per style: other frames give other styles.
        QVERIFY(resolveLayerStyle(p, TitleLayerKind::Text, clip, 6, QSize(640, 270), 1.0)
            != resolveLayerStyle(p, TitleLayerKind::Text, clip, 12, QSize(640, 270), 1.0));
    }

    void projectFormatRoundTrip()
    {
        ParamSet p;
        p.setBase(keys::WriteOnStart, 0.25);
        p.animated(keys::WriteOnEnd, 1.0).setKey(0, 0.0);
        p.animated(keys::WriteOnEnd, 1.0).setKey(30, 1.0);
        const ParamSet back = serialization::paramSetFromJson(serialization::paramSetToJson(p));
        QCOMPARE(back, p);
        QCOMPARE(back.value(keys::WriteOnEnd, 15), 0.5);
        QCOMPARE(back.value(keys::WriteOnStart, 0), 0.25);
    }

    void typewriterPreset()
    {
        const EffectDescriptor* d = EffectRegistry::instance().find(titles::Typewriter);
        QVERIFY(d);
        QCOMPARE(d->category, EffectCategory::Title);
        QVERIFY(d->param(keys::WriteOnEnd));
        const auto clip = effectactions::makeGeneratorClip(
            titles::Typewriter, Rational {25, 1}, QSizeF(1920, 1080));
        QVERIFY(clip);
        const AnimatedValue& end = clip->title.values.value(keys::WriteOnEnd);
        QVERIFY(end.isAnimated());
        QCOMPARE(end.valueAt(0), 0.0);
        QCOMPARE(end.valueAt(25), 1.0);
    }
};

QTEST_MAIN(TestWriteOn)
#include "test_writeon.moc"
