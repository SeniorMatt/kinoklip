// SPDX-License-Identifier: GPL-3.0-or-later
// The Render Queue's thin progress line actually shows the progress (owner report on 0.4.0).
#include "ui/common/ThinProgressBar.h"

#include <QApplication>
#include <QImage>
#include <QTest>

using namespace kk;

class TestThinProgressBar : public QObject
{
    Q_OBJECT
private slots:
    void fillFollowsTheValue()
    {
        for (const QString styleName : {QStringLiteral("Fusion"), QStringLiteral("Breeze")}) {
            QWidget host;
            if (!QApplication::setStyle(styleName) && styleName != QLatin1String("Fusion"))
                continue; // Breeze is not installed everywhere (the bundle ships it)
            ThinProgressBar bar(&host);
            bar.setRange(0, 1000);
            bar.resize(400, bar.sizeHint().height());
            bar.setValue(840);
            QCOMPARE(bar.fraction(), 0.84);
            const QImage image = bar.grab().toImage();
            const QColor fill = bar.palette().color(QPalette::Active, QPalette::Highlight);
            const int y = image.height() / 2;
            auto near = [](QColor a, QColor b) {
                return std::abs(a.red() - b.red()) < 12 && std::abs(a.green() - b.green()) < 12
                    && std::abs(a.blue() - b.blue()) < 12;
            };
            QVERIFY2(near(image.pixelColor(int(image.width() * 0.5), y), fill),
                qPrintable(styleName));
            QVERIFY2(near(image.pixelColor(int(image.width() * 0.8), y), fill),
                qPrintable(styleName));
            QVERIFY2(!near(image.pixelColor(int(image.width() * 0.95), y), fill),
                qPrintable(styleName));
            bar.setValue(0);
            QVERIFY(!near(bar.grab().toImage().pixelColor(image.width() / 2, y), fill));
        }
    }
};

QTEST_MAIN(TestThinProgressBar)
#include "test_thinprogressbar.moc"
