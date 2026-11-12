// SPDX-License-Identifier: GPL-3.0-or-later
// The any-subject model of Background Removal (spec 78.9): its catalog entry, the choice of model
// (automatic by clip type, fixed by the effect's Model parameter), still pictures going to the
// image model with one matte each, the picture maths around the model, and, where the runtime and
// the real file are present (KK_BIREFNET_MODEL or the downloaded model), a real cut-out of a
// synthetic product on white. KK_BGCOMPARE_DIR (a folder of pictures) makes the real-model test
// also write the mattes of both models and the time per picture there, for judging by eye.
#include "matting/MatteBackend.h"
#include "matting/MatteMath.h"
#include "matting/MatteService.h"
#include "matting/ModelCatalog.h"

#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace kk::matting;

namespace {

std::atomic<int> g_imageRuns {0}, g_videoRuns {0};

class CountingBackend final : public MatteBackend
{
public:
    explicit CountingBackend(int value, std::atomic<int>& counter)
        : m_value(value)
        , m_counter(counter)
    { }
    QImage process(const QImage& rgb, MatteQuality, bool) override
    {
        ++m_counter;
        QImage m(rgb.size(), QImage::Format_Grayscale8);
        m.fill(uchar(m_value));
        return m;
    }

private:
    int m_value;
    std::atomic<int>& m_counter;
};

QImage frame(const QString&, int64_t, int)
{
    QImage i(64, 36, QImage::Format_RGB32);
    i.fill(Qt::white);
    return i;
}

/// A grey "laptop" on white: the case the video model cannot do.
QImage productOnWhite(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB888);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(120, 120, 125), w / 120.0));
    p.setBrush(QColor(235, 235, 238));
    p.drawRoundedRect(QRectF(w * 0.2, h * 0.2, w * 0.6, h * 0.45), 12, 12);
    p.setBrush(QColor(40, 42, 48));
    p.drawRect(QRectF(w * 0.23, h * 0.24, w * 0.54, h * 0.37));
    p.setBrush(QColor(200, 200, 205));
    p.drawRoundedRect(QRectF(w * 0.12, h * 0.66, w * 0.76, h * 0.07), 8, 8);
    return img;
}

} // namespace

class TestBirefnet : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;

    static QString realModel()
    {
        const QString env = qEnvironmentVariable("KK_BIREFNET_MODEL");
        if (!env.isEmpty())
            return env;
        return isInstalled(imageModel()) ? modelPath(imageModel()) : QString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        setModelsDirectoryForTests(m_dir.path());
        MatteService::instance().setCacheRoot(m_dir.path() + QStringLiteral("/cache"));
    }
    void cleanupTestCase() { setModelsDirectoryForTests({}); }

    void theCatalogEntryIsCompleteAndOpenSource()
    {
        const ModelInfo& m = imageModel();
        QVERIFY(!m.id.isEmpty() && m.id != defaultModel().id);
        QVERIFY(
            m.fileName.endsWith(QStringLiteral(".onnx")) && m.fileName != defaultModel().fileName);
        QCOMPARE(m.sha256.size(), 64);
        QCOMPARE(m.sha256, m.sha256.toLower());
        QVERIFY(m.sizeBytes > 100000000);
        QCOMPARE(m.licence, QStringLiteral("MIT"));
        QVERIFY(isAllowedDownloadUrl(m.url));
        QVERIFY(m.url.toString().contains(QStringLiteral("ZhengPeng7/BiRefNet")));
        QVERIFY(m.licenceUrl.isValid() && m.sourceUrl.isValid());
        QVERIFY(!m.memoryNote.isEmpty());
        QCOMPARE(&modelInfo(MatteModel::Image), &imageModel());
        QCOMPARE(&modelInfo(MatteModel::Video), &defaultModel());
        QVERIFY(modelPath(m) != modelPath(defaultModel()));
    }

    void theModelFollowsTheChoiceAndTheClipType()
    {
        QCOMPARE(resolveModel(QStringLiteral("auto"), true), MatteModel::Image);
        QCOMPARE(resolveModel(QStringLiteral("auto"), false), MatteModel::Video);
        QCOMPARE(resolveModel(QString(), true), MatteModel::Image); // a project from before: auto
        QCOMPARE(resolveModel(QStringLiteral("video"), true), MatteModel::Video);
        QCOMPARE(resolveModel(QStringLiteral("image"), false), MatteModel::Image);
        QCOMPARE(resolveModel(QStringLiteral("nonsense"), false), MatteModel::Video);
        QVERIFY(isStillImagePath(QStringLiteral("/a/b/photo.PNG")));
        QVERIFY(isStillImagePath(QStringLiteral("shot.jpg")));
        QVERIFY(!isStillImagePath(QStringLiteral("clip.mp4")));
        QVERIFY(inputEdge(MatteQuality::Preview, MatteModel::Image) >= 1024);
        QVERIFY(inputEdge(MatteQuality::Full, MatteModel::Image)
            > inputEdge(MatteQuality::Preview, MatteModel::Image));
        QCOMPARE(inputEdge(MatteQuality::Full), 1280); // the video model is unchanged
    }

    static void settle()
    {
        auto& svc = MatteService::instance();
        svc.unload();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.busy(), 5000);
        QTest::qWait(100); // a job that was already running ends
    }

    void eachModelHasItsOwnSessionAndItsOwnCache()
    {
        auto& svc = MatteService::instance();
        settle();
        g_imageRuns = 0;
        g_videoRuns = 0;
        svc.setFrameSource(frame);
        svc.setBackendFactory([](const QString& path, QString*) -> std::unique_ptr<MatteBackend> {
            return path.endsWith(imageModel().fileName)
                ? std::unique_ptr<MatteBackend>(new CountingBackend(200, g_imageRuns))
                : std::unique_ptr<MatteBackend>(new CountingBackend(100, g_videoRuns));
        });
        const QString path = QStringLiteral("clip.mp4");
        const MattePtr video = svc.matte(path, 0.0, MatteQuality::Preview, true, MatteModel::Video);
        const MattePtr image = svc.matte(path, 0.0, MatteQuality::Preview, true, MatteModel::Image);
        QVERIFY(video && image);
        QCOMPARE(video->constScanLine(0)[0], uchar(100));
        QCOMPARE(image->constScanLine(0)[0], uchar(200));
        QVERIFY(g_videoRuns >= 1 && g_imageRuns >= 1);
        // The same request again comes from the cache, not the model.
        QTRY_VERIFY_WITH_TIMEOUT(!svc.busy(), 5000); // the look-ahead frames are done
        QTest::qWait(50);
        const int before = g_imageRuns;
        const MattePtr again = svc.matte(path, 0.0, MatteQuality::Preview, true, MatteModel::Image);
        QVERIFY(again);
        QCOMPARE(g_imageRuns.load(), before);
        QVERIFY(svc.cachedFrames(path, MatteQuality::Preview, MatteModel::Image) >= 1);
        settle();
    }

    void aStillPictureGetsOneMatteWhateverTheSourceTime()
    {
        auto& svc = MatteService::instance();
        settle();
        g_imageRuns = 0;
        svc.setFrameSource(frame);
        svc.setBackendFactory([](const QString&, QString*) -> std::unique_ptr<MatteBackend> {
            return std::unique_ptr<MatteBackend>(new CountingBackend(255, g_imageRuns));
        });
        const QString path = QStringLiteral("photo.png");
        for (double t : {0.0, 1.0, 2.5, 7.0})
            QVERIFY(svc.matte(path, t, MatteQuality::Preview, true, MatteModel::Image));
        QCOMPARE(g_imageRuns.load(), 1);
        settle();
    }

    void aMissingModelIsReportedPerModel()
    {
        auto& svc = MatteService::instance();
        svc.setBackendFactory({});
        svc.unload();
        if (!runtimeCompiledIn())
            QSKIP("built without ONNX Runtime");
        QVERIFY(!svc.status(MatteModel::Image).ready);
        QVERIFY(!svc.status(MatteModel::Image).modelInstalled);
        QVERIFY(!svc.status(MatteModel::Image).reason.isEmpty());
    }

    void theInputIsSquashedAndNormalised()
    {
        QImage img(8, 4, QImage::Format_RGB888);
        img.fill(QColor(255, 255, 255));
        std::vector<float> v;
        birefnetInput(img, 16, v);
        QCOMPARE(v.size(), size_t(3 * 16 * 16));
        QVERIFY(qAbs(v[0] - (1.f - 0.485f) / 0.229f) < 1e-4f);
        QVERIFY(qAbs(v[256] - (1.f - 0.456f) / 0.224f) < 0.01f);
        QVERIFY(qAbs(v[512] - (1.f - 0.406f) / 0.225f) < 1e-4f);
        img.fill(QColor(0, 0, 0));
        birefnetInput(img, 16, v);
        QVERIFY(qAbs(v[0] + 0.485f / 0.229f) < 1e-4f);
    }

    void logitsBecomeAlpha()
    {
        QCOMPARE(sigmoid(0.f), 0.5f);
        const float values[4] = {-20.f, 0.f, 20.f, 2.f};
        const QImage m = matteFromOutput(values, 2, true);
        QCOMPARE(m.format(), QImage::Format_Grayscale8);
        QCOMPARE(m.constScanLine(0)[0], uchar(0));
        QCOMPARE(m.constScanLine(0)[1], uchar(128));
        QCOMPARE(m.constScanLine(1)[0], uchar(255));
        QVERIFY(m.constScanLine(1)[1] > 220);
        const float plain[4] = {0.f, 0.5f, 1.f, 2.f};
        const QImage p = matteFromOutput(plain, 2, false);
        QCOMPARE(p.constScanLine(0)[1], uchar(128));
        QCOMPARE(p.constScanLine(1)[1], uchar(255)); // clamped
    }

    void refinementSnapsTheMatteToTheEdgeOfThePicture()
    {
        // A picture with a sharp vertical edge at x = 40 and a matte whose edge is 5 px off.
        QImage guide(80, 40, QImage::Format_RGB888);
        QImage matte(80, 40, QImage::Format_Grayscale8);
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 80; ++x) {
                guide.setPixelColor(x, y, x < 40 ? QColor(20, 20, 20) : QColor(235, 235, 235));
                matte.scanLine(y)[x] = x < 45 ? 0 : 255;
            }
        const QImage out = refineMatte(matte, guide, 4, 0.001f);
        QCOMPARE(out.size(), matte.size());
        // Right of the picture's edge the subject (bright) is kept, left of it it is not.
        QVERIFY(out.constScanLine(20)[70] > 240);
        QVERIFY(out.constScanLine(20)[10] < 15);
        // The 5 px error moved towards the picture's edge: pixel 42 was 0 before.
        QVERIFY(out.constScanLine(20)[42] > 60);
        // A flat guide leaves a matte alone (no structure to follow, nothing is invented).
        QImage flat(80, 40, QImage::Format_RGB888);
        flat.fill(Qt::white);
        const QImage same = refineMatte(matte, flat, 4, 0.001f);
        QVERIFY(same.constScanLine(20)[10] < 10 && same.constScanLine(20)[75] > 245);
        QVERIFY(refineRadius(2048) > refineRadius(512));
        QVERIFY(refineRadius(100) >= 2);
    }

    void theRealModelCutsAProductOutOfWhite()
    {
        if (!runtimeCompiledIn())
            QSKIP("built without ONNX Runtime");
        const QString file = realModel();
        if (file.isEmpty())
            QSKIP("the BiRefNet model is not installed (KK_BIREFNET_MODEL)");
        QString why;
        auto backend = createBirefnetBackend(file, &why);
        QVERIFY2(backend, qPrintable(why));
        const QImage img = productOnWhite(512, 384);
        QElapsedTimer timer;
        timer.start();
        const QImage matte = backend->process(img, MatteQuality::Preview, true);
        qInfo() << "BiRefNet:" << timer.elapsed() << "ms for one 512 x 384 picture";
        QCOMPARE(matte.size(), img.size());
        QVERIFY(matte.constScanLine(190)[256] > 200); // the screen
        QVERIFY(matte.constScanLine(5)[5] < 40); // the white corner
        QVERIFY(matte.constScanLine(378)[500] < 40);

        const QString outDir = qEnvironmentVariable("KK_BGCOMPARE_DIR");
        if (outDir.isEmpty())
            return;
        // Every picture of the folder through both models (the video model if it is installed).
        std::unique_ptr<MatteBackend> rvm;
        const QString rvmFile = !qEnvironmentVariable("KK_RVM_MODEL").isEmpty()
            ? qEnvironmentVariable("KK_RVM_MODEL")
            : (isInstalled(defaultModel()) ? modelPath(defaultModel()) : QString());
        if (!rvmFile.isEmpty())
            rvm = createRvmBackend(rvmFile, &why);
        for (const QFileInfo& f :
            QDir(outDir).entryInfoList({QStringLiteral("in-*.*")}, QDir::Files)) {
            QImage in = QImage(f.absoluteFilePath()).convertToFormat(QImage::Format_RGB888);
            if (in.isNull())
                continue;
            const QString name = f.completeBaseName().mid(3);
            QSize s = in.size();
            s.scale(1024, 1024, Qt::KeepAspectRatio);
            QImage a = in.scaled(QSize(s.width() / 4 * 4, s.height() / 4 * 4),
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            timer.restart();
            const QImage m = backend->process(a, MatteQuality::Preview, true);
            qInfo() << name << "BiRefNet" << timer.elapsed() << "ms";
            m.save(outDir + QStringLiteral("/matte-birefnet-") + name + QStringLiteral(".png"));
            a.save(outDir + QStringLiteral("/scaled-") + name + QStringLiteral(".png"));
            if (rvm) {
                QSize r = in.size();
                r.scale(512, 512, Qt::KeepAspectRatio);
                const QImage b = in.scaled(QSize(r.width() / 4 * 4, r.height() / 4 * 4),
                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                timer.restart();
                const QImage mr = rvm->process(b, MatteQuality::Preview, true);
                qInfo() << name << "RVM" << timer.elapsed() << "ms";
                mr.save(outDir + QStringLiteral("/matte-rvm-") + name + QStringLiteral(".png"));
                b.save(outDir + QStringLiteral("/scaledrvm-") + name + QStringLiteral(".png"));
            }
        }
    }
};

QTEST_MAIN(TestBirefnet)
#include "test_birefnet.moc"
