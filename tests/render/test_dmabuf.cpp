// SPDX-License-Identifier: GPL-3.0-or-later
// Zero-copy decoding (spec 77): the DMA-BUF import into GL textures and its fallbacks. A real
// VAAPI surface needs a GPU render node, so the tests cover what does not: the EGL attribute
// lists, the probe's answer on this GL stack, and the download fallback of a frame whose
// buffers cannot be imported (also on a machine with EGL), checked on a real GL texture.
#include "RenderTestUtil.h"
#include "media/DecodeStats.h"
#include "media/VideoDecoder.h"
#include "render/GlDmaBuf.h"
#include "render/GlKit.h"
#include "render/GlVideo.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>

#include <vector>

using namespace kk;
using namespace kk::render;
using namespace kk::test;

namespace {

constexpr int kW = 64;
constexpr int kH = 32;

/// A 64x32 NV12 frame in memory: Y = x, UV = (200, 50).
std::shared_ptr<const media::VideoFrame> cpuNv12()
{
    auto bytes = std::make_shared<std::vector<uint8_t>>(size_t(kW) * kH * 3 / 2);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            (*bytes)[size_t(y) * kW + size_t(x)] = uint8_t(x * 4);
    uint8_t* uv = bytes->data() + size_t(kW) * kH;
    for (int i = 0; i < kW * kH / 4; ++i) {
        uv[2 * i] = 200;
        uv[2 * i + 1] = 50;
    }
    auto frame = std::make_shared<media::VideoFrame>();
    frame->format = media::PixelFormat::Nv12;
    frame->width = kW;
    frame->height = kH;
    frame->planeCount = 2;
    frame->data[0] = bytes->data();
    frame->linesize[0] = kW;
    frame->data[1] = uv;
    frame->linesize[1] = kW;
    frame->storage = bytes;
    return frame;
}

/// A zero-copy frame whose descriptor points at a file descriptor that is no buffer at all
/// (a GPU import fails), with a download() that returns the picture above.
std::shared_ptr<const media::VideoFrame> brokenDmaBufFrame(int* downloads)
{
    auto frame = std::make_shared<media::VideoFrame>();
    frame->format = media::PixelFormat::Nv12;
    frame->width = kW;
    frame->height = kH;
    frame->planeCount = 2;
    auto dmabuf = std::make_shared<media::DmaBufFrame>();
    dmabuf->planeCount = 2;
    dmabuf->planes[0] = {0x20203852, 0, 0, kW, 0, kW, kH};
    dmabuf->planes[1] = {0x38385247, 0, kW * kH, kW, 0, kW / 2, kH / 2};
    dmabuf->download = [downloads] {
        ++*downloads;
        return cpuNv12();
    };
    frame->dmabuf = dmabuf;
    return frame;
}

} // namespace

class TestDmaBuf : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        m_surface = std::make_unique<QOffscreenSurface>();
        m_surface->create();
        m_context = std::make_unique<QOpenGLContext>();
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        m_context->setFormat(format);
        if (!m_context->create() || !m_context->makeCurrent(m_surface.get())) {
            m_context.reset(); // no OpenGL here: the GL cases skip
            return;
        }
        m_gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(m_context.get());
        if (!m_gl || !m_gl->initializeOpenGLFunctions())
            m_context.reset();
    }

    void cleanupTestCase()
    {
        m_context.reset();
        m_surface.reset();
    }

    void anNv12LumaPlaneBecomesTheDocumentedAttributeList()
    {
        const media::DmaBufFrame::Plane plane {
            0x20203852, 17, 256, 4096, 0x0100000000000001ULL, 3840, 2160};
        const std::vector<int32_t> a = dmaBufImageAttributes(plane, true);
        const std::vector<int32_t> expected = {
            0x3057, 3840, // EGL_WIDTH
            0x3056, 2160, // EGL_HEIGHT
            0x3271, 0x20203852, // EGL_LINUX_DRM_FOURCC_EXT: R8
            0x3272, 17, // EGL_DMA_BUF_PLANE0_FD_EXT
            0x3273, 256, // EGL_DMA_BUF_PLANE0_OFFSET_EXT
            0x3274, 4096, // EGL_DMA_BUF_PLANE0_PITCH_EXT
            0x3443, 1, // modifier low
            0x3444, 0x01000000, // modifier high
            0x3038, // EGL_NONE
        };
        QCOMPARE(a, expected);
    }

    void modifiersAreLeftOutWhenUnsupportedOrInvalid()
    {
        media::DmaBufFrame::Plane plane {0x32335247, 5, 0, 7680, 0x0100000000000001ULL, 1920, 1080};
        QCOMPARE(dmaBufImageAttributes(plane, false).size(), size_t(13)); // 6 pairs + EGL_NONE
        plane.modifier = 0x00ffffffffffffffULL; // DRM_FORMAT_MOD_INVALID
        QCOMPARE(dmaBufImageAttributes(plane, true).size(), size_t(13));
        plane.modifier = 0; // linear is a real modifier (DRM_FORMAT_MOD_LINEAR)
        QCOMPARE(dmaBufImageAttributes(plane, true).size(), size_t(17));
        QCOMPARE(dmaBufImageAttributes(plane, true).back(), 0x3038);
    }

    void extensionNamesMatchWholeWords()
    {
        const QString list = QStringLiteral(
            "EGL_KHR_image_base EGL_EXT_image_dma_buf_import_modifiers EGL_MESA_drm_image");
        QVERIFY(hasExtension(list, "EGL_KHR_image_base"));
        QVERIFY(hasExtension(list, "EGL_EXT_image_dma_buf_import_modifiers"));
        QVERIFY(!hasExtension(list, "EGL_EXT_image_dma_buf_import")); // only a prefix of another
        QVERIFY(!hasExtension(QString(), "EGL_KHR_image_base"));
    }

    void planeFormatsHaveNamesForTheLog()
    {
        QCOMPARE(drmFormatName(0x20203852), QStringLiteral("R8"));
        QCOMPARE(drmFormatName(0x38385247), QStringLiteral("GR88"));
        QCOMPARE(drmFormatName(0x20363152), QStringLiteral("R16"));
        QCOMPARE(drmFormatName(0x32335247), QStringLiteral("GR1616"));
        QCOMPARE(drmFormatName(0x1234), QStringLiteral("0x00001234"));
    }

    void theProbeAlwaysAnswersWithAReason()
    {
        if (!m_context)
            QSKIP("no OpenGL 3.3");
        GlDmaBuf importer;
        QString why;
        const bool ok = importer.probe(m_context.get(), &why);
        QCOMPARE(importer.usable(), ok);
        if (!ok)
            QVERIFY2(!why.isEmpty(), "a failed probe must say why (it goes into the log)");
        qInfo("probe on Qt platform \"%s\": %s", qPrintable(QGuiApplication::platformName()),
            ok ? "EGL DMA-BUF import available" : qPrintable(why));
        QString none;
        GlDmaBuf noContext;
        QVERIFY(!noContext.probe(nullptr, &none));
        QVERIFY(!none.isEmpty());
    }

    void aFrameThatCannotBeImportedIsDownloadedInstead()
    {
        if (!m_context)
            QSKIP("no OpenGL 3.3");
        GlKit kit(m_gl);
        QString why;
        GlVideo video;
        QVERIFY2(video.initialize(kit, &why), qPrintable(why));
        GlDmaBuf importer;
        (void)importer.probe(m_context.get(), &why); // usable on EGL stacks, not on GLX
        video.setDmaBufImporter(&importer);

        int downloads = 0;
        PlaneTextures textures;
        const auto frame = brokenDmaBufFrame(&downloads);
        video.upload(kit, textures, frame);
        QCOMPARE(downloads, 1); // the import cannot work with fd 0: the picture came by download
        QCOMPARE(video.importedFrames(), int64_t(0));
        QCOMPARE(video.fallbackFrames(), int64_t(1));
        QVERIFY(!importer.usable()); // and the importer switched itself off
        QCOMPARE(textures.uploaded, frame);
        video.upload(kit, textures, frame); // the same frame again costs nothing
        QCOMPARE(downloads, 1);

        // The luma texture holds the downloaded picture.
        std::vector<uint8_t> luma(size_t(kW) * kH);
        m_gl->glBindTexture(GL_TEXTURE_2D, textures.texture[0]);
        m_gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        m_gl->glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, luma.data());
        QCOMPARE(int(luma[0]), 0);
        QCOMPARE(int(luma[10]), 40);
        QCOMPARE(int(luma[size_t(kW) * kH - 1]), 252);
        GlVideo::release(kit, textures);
        video.destroy(kit);
    }

    void withoutAnImporterTheSameFrameIsDownloaded()
    {
        if (!m_context)
            QSKIP("no OpenGL 3.3");
        GlKit kit(m_gl);
        QString why;
        GlVideo video;
        QVERIFY2(video.initialize(kit, &why), qPrintable(why));
        int downloads = 0;
        PlaneTextures textures;
        video.upload(kit, textures, brokenDmaBufFrame(&downloads));
        QCOMPARE(downloads, 1);
        QVERIFY(textures.texture[0] != 0 && textures.texture[1] != 0);
        GlVideo::release(kit, textures);
        video.destroy(kit);
    }

    void softwareDecodingNeverExportsDmaBufs()
    {
        // Zero-copy only applies to VAAPI surfaces: a software decoder still returns planes.
        QString why;
        if (!ensureFixtures(&why))
            QSKIP(qPrintable(why));
        const QString path = fixture("h264_25fps.mp4");
        media::VideoDecoder decoder;
        decoder.setHwAccel(media::HwAccelMode::Off);
        decoder.setZeroCopy(true);
        QVERIFY(decoder.open(path));
        QVERIFY(!decoder.zeroCopyActive());
        const auto frame = decoder.decodeFrame(0);
        QVERIFY(frame);
        QVERIFY(!frame->dmabuf);
        QVERIFY(frame->data[0] != nullptr);
    }

    void theDecodeWindowIsDescribedForTheStatsLine()
    {
        media::DecodeCounters a;
        media::DecodeCounters b;
        QVERIFY(media::describeDecodeWindow(a, b).isEmpty()); // nothing decoded: no text
        b.zeroCopyFrames = 59;
        b.zeroCopyUs = 59 * 120;
        b.softwareFrames = 1;
        b.importFailures = 2;
        const QString text = media::describeDecodeWindow(a, b);
        QVERIFY2(text.contains(QStringLiteral("59 zero-copy (0.12 ms each)")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("0 downloaded")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("1 software")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("2 GPU imports failed")), qPrintable(text));
    }

private:
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    QOpenGLFunctions_3_3_Core* m_gl = nullptr;
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestDmaBuf>(argc, argv);
}

#include "test_dmabuf.moc"
