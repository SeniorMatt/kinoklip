// SPDX-License-Identifier: GPL-3.0-or-later
// Video scopes (spec 44a): waveform, parade, vectorscope and histogram counts on known frames
// (render/Scopes.h), and the scope widgets drawing them (src/ui/scopes).
#include "render/Scopes.h"
#include "ui/scopes/ScopeView.h"

#include <QApplication>
#include <QTest>

#include <cmath>

using namespace kk;
using namespace kk::scopes;

class TestScopes : public QObject
{
    Q_OBJECT
private slots:
    void solidColourVectorscopePoint()
    {
        QImage red(40, 20, QImage::Format_RGB32);
        red.fill(QColor(255, 0, 0));
        const VectorscopeData v = vectorscope(red, 256);
        // Rec. 709 red: Cb = -0.2126 / 1.8556, Cr = (1 - 0.2126) / 1.5748 = 0.5 (the top edge).
        const QPointF cbCr = chroma(1, 0, 0);
        QVERIFY(std::abs(cbCr.x() - (-0.2126 / 1.8556)) < 1e-6); // float luma weights
        QVERIFY(std::abs(cbCr.y() - 0.5) < 1e-6);
        const QPointF cell = vectorscopeCell(cbCr, 256);
        const int x = int(std::lround(cell.x())), y = int(std::lround(cell.y()));
        QCOMPARE(x, 98);
        QCOMPARE(y, 0);
        QCOMPARE(v.at(x, y), uint32_t(40 * 20));
        // Grey sits in the centre; blue at the right edge (Cb = +0.5).
        QImage grey(10, 10, QImage::Format_RGB32);
        grey.fill(QColor(128, 128, 128));
        const VectorscopeData g = vectorscope(grey, 255);
        QCOMPARE(g.at(127, 127), uint32_t(100));
        QVERIFY(std::abs(chroma(0, 0, 1).x() - 0.5) < 1e-4);
    }

    void rampWaveform()
    {
        // A horizontal luma ramp: column x holds every pixel at level x.
        QImage ramp(256, 16, QImage::Format_RGB32);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 256; ++x)
                ramp.setPixel(x, y, qRgb(x, x, x));
        const WaveformData w = waveform(ramp, false, 256, 256);
        QCOMPARE(w.planes.size(), size_t(1));
        for (const int x : {0, 1, 100, 200, 255}) {
            QCOMPARE(w.at(0, x, x), uint32_t(16));
            QCOMPARE(w.at(0, x, (x + 128) % 256), uint32_t(0));
        }
        // Fewer columns gather neighbouring pixels.
        const WaveformData narrow = waveform(ramp, false, 64, 256);
        QCOMPARE(narrow.columns, 64);
        QCOMPARE(narrow.at(0, 10, 40), uint32_t(16));
        // RGB waveform and parade data: a pure green picture.
        QImage green(8, 8, QImage::Format_RGB32);
        green.fill(QColor(0, 255, 0));
        const WaveformData rgb = waveform(green, true, 8, 256);
        QCOMPARE(rgb.planes.size(), size_t(3));
        QCOMPARE(rgb.at(0, 3, 0), uint32_t(8));
        QCOMPARE(rgb.at(1, 3, 255), uint32_t(8));
        QCOMPARE(rgb.at(2, 3, 0), uint32_t(8));
        // Luma of pure green: 0.7152 x 255 = 182.4 -> level 182.
        QCOMPARE(waveform(green, false, 8, 256).at(0, 0, 182), uint32_t(8));
    }

    void histogramCounts()
    {
        QImage halves(10, 10, QImage::Format_RGB32);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x)
                halves.setPixel(x, y, x < 5 ? qRgb(0, 0, 0) : qRgb(255, 255, 255));
        const HistogramData h = histogram(halves, 256);
        for (int c = 0; c < 4; ++c) {
            QCOMPARE(h.channels[size_t(c)][0], uint32_t(50));
            QCOMPARE(h.channels[size_t(c)][255], uint32_t(50));
        }
        // Transparent pixels count as black (premultiplied over black).
        QImage clear(4, 4, QImage::Format_ARGB32_Premultiplied);
        clear.fill(Qt::transparent);
        QCOMPARE(histogram(clear, 16).channels[3][0], uint32_t(16));
    }

    void scopeInputIsSmall()
    {
        QImage big(1920, 1080, QImage::Format_RGB32);
        big.fill(Qt::blue);
        const QImage small = scopeInput(big, 480);
        QCOMPARE(small.size(), QSize(480, 270));
        QCOMPARE(small.format(), QImage::Format_ARGB32_Premultiplied);
    }

    void viewsDraw()
    {
        // Every scope type draws something for a colourful frame (offscreen widget grab).
        QImage bars(64, 32, QImage::Format_RGB32);
        const QColor colours[] = {
            Qt::white, Qt::yellow, Qt::cyan, Qt::green, Qt::magenta, Qt::red, Qt::blue, Qt::black};
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x)
                bars.setPixelColor(x, y, colours[x / 8]);
        for (const ScopeType t : {ScopeType::WaveformLuma, ScopeType::WaveformRgb,
                 ScopeType::Parade, ScopeType::Vectorscope, ScopeType::Histogram}) {
            ScopeView view;
            view.resize(300, 200);
            view.setType(t);
            QCOMPARE(view.type(), t);
            view.setImage(bars);
            const QImage shot = view.grab().toImage();
            QCOMPARE(shot.size(), QSize(300, 200));
            const QImage trace = view.traceImage();
            QVERIFY2(!trace.isNull(), scopeTypeId(t));
            int lit = 0;
            for (int y = 0; y < trace.height(); ++y)
                for (int x = 0; x < trace.width(); ++x)
                    lit += qGray(trace.pixel(x, y)) > 40 ? 1 : 0;
            // Seven distinct bar colours light seven vectorscope cells (white and black share the
            // centre); the other scopes draw lines.
            QVERIFY2(lit >= (t == ScopeType::Vectorscope ? 7 : 20), scopeTypeId(t));
        }
        QCOMPARE(scopeTypeFromId(QStringLiteral("parade")), ScopeType::Parade);
        QCOMPARE(scopeTypeFromId(QStringLiteral("bogus")), ScopeType::WaveformLuma);
    }
};

QTEST_MAIN(TestScopes)
#include "test_scopes.moc"
