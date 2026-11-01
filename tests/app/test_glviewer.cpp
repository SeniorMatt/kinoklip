// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel test of the viewer surfaces. The GL case is skipped where OpenGL widgets are unavailable
// (ctest runs offscreen); run it with GL, e.g.:
//   QT_QPA_PLATFORM=xcb xvfb-run -a ./build/test_glviewer
#include "ui/viewer/GlViewerSurface.h"
#include "ui/viewer/RasterViewerSurface.h"

#include <QApplication>
#include <QSurfaceFormat>
#include <QTest>

using namespace kk;

namespace {

QImage testPattern()
{
    // Left half red, right half blue, 16:9.
    QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = image.width() / 2; x < image.width(); ++x)
            line[x] = qRgb(0, 0, 255);
    }
    return image;
}

void verifyPattern(const QImage& shot, const QRect& picture)
{
    const QPoint left(picture.left() + picture.width() / 4, picture.center().y());
    const QPoint right(picture.left() + 3 * picture.width() / 4, picture.center().y());
    const QColor l = shot.pixelColor(left);
    const QColor r = shot.pixelColor(right);
    QVERIFY2(l.red() > 200 && l.blue() < 50, qPrintable(l.name()));
    QVERIFY2(r.blue() > 200 && r.red() < 50, qPrintable(r.name()));
    const QColor corner = shot.pixelColor(2, 2); // surround, not picture
    QVERIFY(corner.red() < 30 && corner.blue() < 30);
}

} // namespace

class TestGlViewer : public QObject
{
    Q_OBJECT
private slots:
    void raster()
    {
        RasterViewerSurface surface;
        surface.resize(640, 400);
        surface.setFrame(testPattern());
        verifyPattern(surface.grab().toImage(), surface.pictureRect());
    }

    void openGL()
    {
        if (!ViewerSurface::openGLAvailable())
            QSKIP("OpenGL widgets not available on this platform");
        GlViewerSurface surface;
        surface.resize(640, 400);
        surface.show();
        QVERIFY(QTest::qWaitForWindowExposed(&surface));
        surface.setFrame(testPattern());
        verifyPattern(surface.grabFramebuffer(), surface.pictureRect());
        // Same-size frames reuse the texture (glTexSubImage2D path).
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        QImage swapped = testPattern().flipped(Qt::Horizontal); // mirrored() is deprecated
#else
        QImage swapped = testPattern().mirrored(true, false);
#endif
        surface.setFrame(swapped);
        const QImage shot = surface.grabFramebuffer();
        const QRect p = surface.pictureRect();
        QVERIFY(shot.pixelColor(p.left() + p.width() / 4, p.center().y()).blue() > 200);
    }
};

int main(int argc, char** argv)
{
    // Same default format as the application (OpenGL 3.3 core), set before QApplication.
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    TestGlViewer test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_glviewer.moc"
