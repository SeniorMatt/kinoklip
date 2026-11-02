// SPDX-License-Identifier: GPL-3.0-or-later
// Export speed: 1080p25 H.264 with the software encoder (libx264) and every available hardware
// encoder (NVENC / VAAPI / QSV), (a) encoder + pixel conversion alone on pre-rendered frames,
// (b) the whole pipeline (compositor, conversion, encoding, AAC audio, muxing). Prints fps; it
// asserts only that every run succeeds (speed depends on the machine).
#include "ExportTestUtil.h"
#include "export/EncoderCatalog.h"
#include "export/ExportJob.h"
#include "export/VideoEncoder.h"
#include "render/Compositor.h"

#include <QElapsedTimer>

using namespace kk;
using namespace kk::test;

namespace {
constexpr int kFrames = 100; ///< frames per measurement (4 s at 25 fps)
constexpr int kDistinctFrames = 10; ///< pre-rendered frames cycled in the encode-only case
} // namespace

class TestExportPerf : public QObject
{
    Q_OBJECT

    QList<EncoderChoice> h264Encoders()
    {
        QList<EncoderChoice> list = EncoderCatalog::softwareEncoders(QStringLiteral("h264"));
        for (const HwEncoderStatus& hw : EncoderCatalog::hardwareEncoders()) {
            if (hw.available && hw.codecId == QLatin1String("h264")) {
                list.push_back({hw.encoderName, hw.backend});
            }
        }
        return list;
    }

private Q_SLOTS:
    void speed()
    {
        QString why;
        if (!ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
        Project project;
        const ImportResult imported
            = project.importMedia({fixture("perf_1080p.mp4"), fixture("h264_25fps.mp4")});
        QCOMPARE(imported.importedIds.size(), 2);
        project.beginEdit(QStringLiteral("perf"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 1920;
        tl.height = 1080;
        tl.fps = {25, 1};
        addClip(tl, TrackType::Video, 0,
            mediaClip(tl, imported.importedIds[0], 0, kFrames, 0, 10'000'000));
        addClip(tl, TrackType::Audio, 0,
            mediaClip(tl, imported.importedIds[1], 0, kFrames, 0, 10'000'000));
        project.endEdit();
        const auto snapshot = project.timeline()->snapshot();
        const auto media = makeMediaPathMap(project.mediaPool()->state());

        Compositor compositor;
        qInfo("compositor: %s", qPrintable(compositor.backendDescription()));
        std::vector<QImage> frames;
        for (int i = 0; i < kDistinctFrames; ++i) {
            frames.push_back(compositor.renderImage(snapshot, i * 7));
            QVERIFY(!frames.back().isNull());
        }
        QTemporaryDir dir;
        for (const EncoderChoice& choice : h264Encoders()) {
            // (a) encoder + conversion only.
            exporter::VideoEncoderConfig config;
            config.encoderName = choice.encoderName;
            config.backend = choice.backend;
            config.settings.codec = QStringLiteral("h264");
            config.settings.quality = 20;
            config.size = {1920, 1080};
            config.fps = {25, 1};
            AVBufferRef* device = nullptr;
            if (choice.backend == EncoderBackend::Vaapi) {
                device = EncoderCatalog::vaapiDevice(choice.encoderName, &config.lowPower);
                config.hwDevice = device;
            }
            exporter::VideoEncoder encoder;
            QString error;
            QVERIFY2(encoder.open(config, &error), qPrintable(error));
            int64_t bytes = 0;
            const exporter::PacketSink sink = [&](AVPacket* p) {
                bytes += p->size;
                return true;
            };
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < kFrames; ++i) {
                QVERIFY2(encoder.encode(frames[size_t(i % kDistinctFrames)], i, sink, &error),
                    qPrintable(error));
            }
            QVERIFY2(encoder.flush(sink, &error), qPrintable(error));
            const double encodeFps = kFrames / (double(timer.nsecsElapsed()) / 1e9);
            av_buffer_unref(&device);

            // (b) full pipeline.
            ExportSettings s;
            s.container = QStringLiteral("mp4");
            s.video.codec = QStringLiteral("h264");
            s.video.encoder = choice.backend;
            s.video.quality = 20;
            s.outputPath = dir.filePath(choice.encoderName + QStringLiteral(".mp4"));
            ExportJob job(s, snapshot, media);
            job.setCompositor(&compositor);
            const ExportResult result = job.run();
            QVERIFY2(result.ok, qPrintable(result.error));
            QCOMPARE(result.videoEncoder, choice.encoderName);
            std::printf("EXPORT SPEED 1080p25 H.264 %-11s encode-only %6.1f fps (conversion %.2f "
                        "ms/frame) | "
                        "full pipeline %6.1f fps\n",
                qPrintable(choice.encoderName), encodeFps, encoder.convertMs() / kFrames,
                result.averageFps());
            std::fflush(stdout);
        }
    }
};

int main(int argc, char** argv)
{
    // Offscreen platform = the QPainter compositor (~7 ms/frame at 1080p). Under xvfb the GL
    // path would run on llvmpipe (software GL), which is slower than the CPU backend.
    QGuiApplication app(argc, argv);
    TestExportPerf test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_exportperf.moc"
