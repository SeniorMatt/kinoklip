// SPDX-License-Identifier: GPL-3.0-or-later
// The Robust Video Matting session (spec 78): refuses a file that is not the model without
// crashing, and, where the runtime and the real model are present (KK_RVM_MODEL, or the model
// downloaded in Preferences), cuts out a synthetic picture and keeps its state between frames.
#include "matting/MatteBackend.h"
#include "matting/ModelCatalog.h"

#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace kk::matting;

class TestRvmBackend : public QObject
{
    Q_OBJECT
    static QString realModel()
    {
        const QString env = qEnvironmentVariable("KK_RVM_MODEL");
        if (!env.isEmpty())
            return env;
        return isInstalled(defaultModel()) ? modelPath(defaultModel()) : QString();
    }

private slots:
    void sizesFollowTheQuality()
    {
        QVERIFY(inputEdge(MatteQuality::Full) > inputEdge(MatteQuality::Preview));
        const float r = downsampleRatio(MatteQuality::Preview, 512);
        QVERIFY(r > 0.1f && r <= 1.0f);
        QVERIFY(downsampleRatio(MatteQuality::Preview, 64) <= 1.0f);
    }

    void aFileThatIsNotTheModelIsRefusedWithAReason()
    {
        QTemporaryDir dir;
        const QString bad = dir.path() + QStringLiteral("/bad.onnx");
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not an onnx file");
        f.close();
        QString why;
        QVERIFY(!createRvmBackend(bad, &why));
        QVERIFY(!why.isEmpty());
        QString why2;
        QVERIFY(!createRvmBackend(dir.path() + QStringLiteral("/missing.onnx"), &why2));
        QVERIFY(!why2.isEmpty());
        if (!runtimeCompiledIn())
            QVERIFY(runtimeVersion().isEmpty());
        else
            QVERIFY(runtimeVersion().startsWith(QStringLiteral("ONNX Runtime")));
    }

    void theRealModelCutsOutAPictureAndKeepsItsState()
    {
        if (!runtimeCompiledIn())
            QSKIP("built without ONNX Runtime");
        const QString path = realModel();
        if (path.isEmpty())
            QSKIP("no model: set KK_RVM_MODEL or download it in Preferences");
        QString why;
        auto backend = createRvmBackend(path, &why);
        QVERIFY2(backend, qPrintable(why));
        QImage pic(256, 144, QImage::Format_RGB888);
        pic.fill(QColor(40, 90, 40));
        {
            QPainter p(&pic);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(210, 160, 130));
            p.drawEllipse(QRect(100, 20, 56, 56)); // a head
            p.setBrush(QColor(30, 60, 200));
            p.drawRect(QRect(76, 76, 104, 68)); // shoulders
        }
        const QImage first = backend->process(pic, MatteQuality::Preview, true);
        QCOMPARE(first.size(), pic.size());
        QCOMPARE(first.format(), QImage::Format_Grayscale8);
        // Frames that follow keep working with the carried state.
        QImage again;
        for (int i = 0; i < 3; ++i)
            again = backend->process(pic, MatteQuality::Preview, false);
        QCOMPARE(again.size(), pic.size());
        // Background corner stays low in the matte.
        QVERIFY(qGray(again.pixel(4, 4)) < 128);
    }

    void speedAtPreviewAndFullQuality()
    {
        if (!runtimeCompiledIn())
            QSKIP("built without ONNX Runtime");
        const QString path = realModel();
        if (path.isEmpty())
            QSKIP("no model: set KK_RVM_MODEL or download it in Preferences");
        QString why;
        auto backend = createRvmBackend(path, &why);
        QVERIFY2(backend, qPrintable(why));
        // A 1920 x 1080 frame brought to the sizes the service feeds the model.
        QImage hd(1920, 1080, QImage::Format_RGB888);
        hd.fill(QColor(60, 100, 60));
        {
            QPainter p(&hd);
            p.setBrush(QColor(200, 150, 120));
            p.drawEllipse(QRect(800, 150, 300, 300));
            p.setBrush(QColor(40, 60, 190));
            p.drawRect(QRect(650, 450, 600, 630));
        }
        for (MatteQuality q : {MatteQuality::Preview, MatteQuality::Full}) {
            QSize size = hd.size();
            size.scale(inputEdge(q), inputEdge(q), Qt::KeepAspectRatio);
            size = QSize((size.width() + 2) / 4 * 4, (size.height() + 2) / 4 * 4);
            const QImage in = hd.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                  .convertToFormat(QImage::Format_RGB888);
            backend->process(in, q, true); // warm-up
            QElapsedTimer timer;
            timer.start();
            const int n = 10;
            for (int i = 0; i < n; ++i)
                QVERIFY(!backend->process(in, q, false).isNull());
            qInfo("%s %dx%d: %.1f ms per frame on %d threads",
                q == MatteQuality::Full ? "full" : "preview", in.width(), in.height(),
                double(timer.nsecsElapsed()) / 1e6 / n, QThread::idealThreadCount() / 2);
        }
    }
};

QTEST_MAIN(TestRvmBackend)
#include "test_rvmbackend.moc"
