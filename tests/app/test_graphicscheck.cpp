// SPDX-License-Identifier: GPL-3.0-or-later
// `kinoklip --graphics-check` (spec 177): the pure parts (software renderer names, the test
// picture and its comparison), the report of a run, drawing with a real context when the test
// platform has OpenGL, and the command-line switch.
#include "app/CommandLine.h"
#include "app/GraphicsCheck.h"

#include <QFileInfo>
#include <QImage>
#include <QOpenGLContext>
#include <QTest>

using namespace kk;
using namespace kk::graphicscheck;

namespace {

/// What a correct card reads back: expectedPictureColor() with the image's row order.
QImage correctPicture()
{
    QImage image(kPictureSize, kPictureSize, QImage::Format_RGBA8888);
    for (int y = 0; y < kPictureSize; ++y) {
        for (int x = 0; x < kPictureSize; ++x)
            image.setPixelColor(x, y, expectedPictureColor(x, kPictureSize - 1 - y));
    }
    return image;
}

} // namespace

class TestGraphicsCheck : public QObject
{
    Q_OBJECT

private slots:
    void softwareRenderersAreRecognised()
    {
        for (const char* name : {"llvmpipe (LLVM 15.0.7, 256 bits)", "softpipe", "Mesa Software Rasterizer",
                 "Gallium 0.4 on llvmpipe", "SwiftShader Device", "Microsoft Basic Render Driver"})
            QVERIFY2(isSoftwareRenderer(QLatin1String(name)), name);
        for (const char* name : {"Mesa Intel(R) Iris(R) Xe Graphics (TGL GT2)", "NVIDIA GeForce RTX 3060/PCIe/SSE2",
                 "AMD Radeon RX 6600 (radeonsi, navi23, LLVM 17.0.6, DRM 3.54)", "V3D 7.1.7", ""})
            QVERIFY2(!isSoftwareRenderer(QLatin1String(name)), name);
    }

    void expectedColourFollowsPosition()
    {
        QCOMPARE(expectedPictureColor(0, 0), QColor(2, 2, 128));
        QCOMPARE(expectedPictureColor(63, 63), QColor(253, 253, 128));
        QVERIFY(expectedPictureColor(32, 0).red() > expectedPictureColor(31, 0).red());
        QVERIFY(expectedPictureColor(0, 32).green() > expectedPictureColor(0, 31).green());
    }

    void pictureComparison()
    {
        QString problem;
        QImage image = correctPicture();
        QVERIFY2(pictureMatches(image, &problem), qPrintable(problem));
        // Rounding of a few levels is fine.
        QColor c = image.pixelColor(16, 16);
        c.setRed(c.red() + 3);
        image.setPixelColor(16, 16, c);
        QVERIFY(pictureMatches(image, &problem));
        // A real difference is named.
        c.setRed(c.red() + 40);
        image.setPixelColor(16, 16, c);
        QVERIFY(!pictureMatches(image, &problem));
        QVERIFY2(problem.contains(QStringLiteral("(16, 16)")), qPrintable(problem));
        // Upside down: the read-back of a card that flips rows.
        QVERIFY(!pictureMatches(correctPicture().flipped(Qt::Vertical), &problem));
        // A missed draw leaves the clear colour.
        QImage magenta(kPictureSize, kPictureSize, QImage::Format_RGBA8888);
        magenta.fill(Qt::magenta);
        QVERIFY(!pictureMatches(magenta, &problem));
        // Wrong size, empty.
        QVERIFY(!pictureMatches(QImage(32, 32, QImage::Format_RGBA8888), &problem));
        QVERIFY(problem.contains(QStringLiteral("32x32")));
        QVERIFY(!pictureMatches(QImage(), &problem));
    }

    void reportHasEverySection()
    {
        const Report report = run();
        for (const char* text : {"Kinoklip graphics check", "Platform plugin:", "OpenGL",
                 "Hardware video decoding", "GPU render nodes:", "VAAPI", "CUDA", "Vulkan",
                 "Verdict:"})
            QVERIFY2(report.text.contains(QLatin1String(text)), text);
        // The verdict follows the findings.
        QCOMPARE(report.ok, report.glContext && report.pictureCorrect);
        if (report.ok) {
            QVERIFY(report.pictureDrawn);
            QVERIFY(!report.renderer.isEmpty());
            QVERIFY(report.text.contains(report.renderer));
            QVERIFY(report.text.contains(QStringLiteral("correct")));
        } else {
            QVERIFY(report.text.contains(QStringLiteral("did NOT draw")));
            QVERIFY(!report.pictureProblem.isEmpty());
        }
        QCOMPARE(report.hardwareDecoding, !report.hardwareBackends.isEmpty());
    }

    void realContextDrawsThePicture()
    {
        // Needs a platform with OpenGL (a desktop session, xvfb or Mesa on the offscreen platform).
        QOpenGLContext probe;
        if (!probe.create())
            QSKIP("this test platform has no OpenGL");
        Report report;
        QVERIFY(drawTestPicture(&report));
        QVERIFY(report.glContext);
        QVERIFY(!report.glApi.isEmpty());
        QVERIFY(!report.renderer.isEmpty());
        QVERIFY(report.pictureDrawn);
        QVERIFY2(report.pictureCorrect, qPrintable(report.pictureProblem));
    }

    void videoFileIsDecodedWithEveryBackend()
    {
        const QString video = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4");
        if (!QFileInfo::exists(video))
            QSKIP("test media missing (tests/data/generated)");
        Report report;
        checkVideoDecoding(video, &report);
        QVERIFY2(report.text.contains(QStringLiteral("Decoding h264_25fps.mp4")),
            qPrintable(report.text));
        QVERIFY2(report.text.contains(QStringLiteral("Off (software):")), qPrintable(report.text));
        QVERIFY(report.text.contains(QStringLiteral("Auto:")));
        QVERIFY2(!report.videoResult.isEmpty(), qPrintable(report.text));
        // A file that is not there is said, not hidden.
        Report missing;
        checkVideoDecoding(QStringLiteral("/nonexistent/clip.mp4"), &missing);
        QVERIFY2(missing.text.contains(QStringLiteral("cannot open")), qPrintable(missing.text));
    }

    void switchIsParsed()
    {
        CommandLineOptions options;
        QString error;
        QVERIFY(parseCommandLine({QStringLiteral("kinoklip"), QStringLiteral("--graphics-check")},
            options, &error));
        QVERIFY(options.graphicsCheck);
        QVERIFY(options.graphicsCheckFile.isEmpty());
        QVERIFY(parseCommandLine({QStringLiteral("kinoklip"), QStringLiteral("--graphics-check"),
                                     QStringLiteral("clip.mp4")},
            options, &error));
        QCOMPARE(options.graphicsCheckFile, QStringLiteral("clip.mp4"));
        CommandLineOptions bad;
        QVERIFY(!parseCommandLine({QStringLiteral("kinoklip"), QStringLiteral("--graphics-check"),
                                      QStringLiteral("a"), QStringLiteral("b")},
            bad, &error));
        QVERIFY(error.contains(QStringLiteral("--graphics-check")));
        CommandLineOptions none;
        QVERIFY(parseCommandLine({QStringLiteral("kinoklip")}, none, &error));
        QVERIFY(!none.graphicsCheck);
    }
};

QTEST_MAIN(TestGraphicsCheck)
#include "test_graphicscheck.moc"
