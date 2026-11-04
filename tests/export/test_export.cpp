// SPDX-License-Identifier: GPL-3.0-or-later
// Export engine tests: every built-in preset renders the fixture project (320x180, 2 s, two
// video tracks, title, dissolve, audio) and the result is probed and decoded; hardware
// encoders when available; A/V sync (flash frame vs audio impulse); cancel; settings JSON;
// user presets; the render queue; the --render command line.
#include "ExportTestUtil.h"
#include "export/EncoderCatalog.h"
#include "export/ExportFFmpeg.h"
#include "export/ExportJob.h"
#include "export/ExportPresets.h"
#include "export/RenderQueue.h"
#include "export/RgbToYuv.h"
#include "render/Compositor.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>

using namespace kk;
using namespace kk::test;

namespace {

/// One frame at 25 fps in microseconds: the duration tolerance.
constexpr int64_t kFrameUs = 40'000;
constexpr int64_t kProjectUs = kProjectFrames * kFrameUs;

/// FFmpeg codec name a probe reports for a VideoCodecInfo / AudioCodecInfo id.
QString probedVideoCodec(const QString& id)
{
    return id == QLatin1String("dnxhr") ? QStringLiteral("dnxhd") : id;
}

QString probedAudioCodec(const QString& id)
{
    return id;
}

} // namespace

class TestExport : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Project> m_project;
    std::unique_ptr<Compositor> m_compositor;
    QString m_skip;

    ExportResult render(ExportSettings settings, const ExportJob::ProgressCallback& progress = {})
    {
        ExportJob job(settings, m_project->timeline()->snapshot(),
            makeMediaPathMap(m_project->mediaPool()->state()));
        job.setCompositor(m_compositor.get());
        return job.run(progress);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_project = std::make_unique<Project>();
        if (!buildExportProject(*m_project, &m_skip)) {
            return;
        }
        m_compositor = std::make_unique<Compositor>();
        qInfo("compositor: %s", qPrintable(m_compositor->backendDescription()));
    }

    void cleanupTestCase() { m_compositor.reset(); }

    void settingsJson()
    {
        ExportSettings s;
        s.container = QStringLiteral("mov");
        s.video.codec = QStringLiteral("prores");
        s.video.profile = QStringLiteral("4444");
        s.video.alpha = true;
        s.video.encoder = EncoderBackend::Vaapi;
        s.video.rateControl = RateControl::Bitrate;
        s.audio.codec = QStringLiteral("pcm_s24le");
        s.range = RangeMode::Custom;
        s.rangeIn = 5;
        s.rangeOut = 17;
        s.video.keyframeInterval = 12;
        s.video.frameReordering = false;
        s.outputPath = QStringLiteral("/tmp/x.mov");
        const QByteArray json = QJsonDocument(s.toJson()).toJson();
        const ExportSettings back
            = ExportSettings::fromJson(QJsonDocument::fromJson(json).object(), ExportSettings {});
        QCOMPARE(back, s);
        QVERIFY(back.keepsAlpha());
        QString error;
        QVERIFY2(back.validate(&error), qPrintable(error));

        ExportSettings bad = s;
        bad.container = QStringLiteral("webm"); // ProRes in WebM is not allowed
        QVERIFY(!bad.validate(&error));
        bad.normalize();
        QCOMPARE(bad.video.codec, QStringLiteral("vp9"));
        QCOMPARE(bad.audio.codec, QStringLiteral("opus"));
        QCOMPARE(bad.outputPath, QStringLiteral("/tmp/x.webm"));
        QVERIFY2(bad.validate(&error), qPrintable(error));

        QCOMPARE(
            sequencePattern(QStringLiteral("/a/shot.png")), QStringLiteral("/a/shot_%05d.png"));
        QCOMPARE(sequencePattern(QStringLiteral("/a/s_%03d.png")), QStringLiteral("/a/s_%03d.png"));

        TimelineData tl = emptyTimeline(1920, 1080);
        ExportSettings gif;
        gif.container = QStringLiteral("gif");
        gif.video.codec = QStringLiteral("gif");
        gif.video.width = 480;
        QCOMPARE(gif.outputSize(tl), QSize(480, 270));
        ExportSettings odd;
        odd.video.width = 321;
        odd.video.height = 181;
        QCOMPARE(odd.outputSize(tl), QSize(320, 180)); // 4:2:0 needs even sizes
    }

    void fastYuvMatchesSwscale()
    {
        // The own BT.709 converter must agree with swscale (accurate rounding) within 1-2 codes.
        constexpr int w = 64;
        constexpr int h = 32;
        QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
        uint32_t seed = 12345;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                seed = seed * 1664525u + 1013904223u;
                image.setPixel(x, y, 0xff000000u | (seed >> 8));
            }
        }
        std::vector<uint8_t> y1(w * h), u1(w * h / 4), v1(w * h / 4), y2(w * h), u2(w * h / 4),
            v2(w * h / 4);
        exporter::rgb32ToYuv420(image.constBits(), int(image.bytesPerLine()), w, 0, h,
            {y1.data(), w, u1.data(), w / 2, v1.data(), w / 2, false});
        SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_RGB32, w, h, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP, nullptr, nullptr, nullptr);
        sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_DEFAULT), 1,
            sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
        const uint8_t* src[] = {image.constBits()};
        const int srcStride[] = {int(image.bytesPerLine())};
        uint8_t* dst[] = {y2.data(), u2.data(), v2.data()};
        const int dstStride[] = {w, w / 2, w / 2};
        sws_scale(sws, src, srcStride, 0, h, dst, dstStride);
        sws_freeContext(sws);
        int maxY = 0;
        for (size_t i = 0; i < y1.size(); ++i) {
            maxY = std::max(maxY, std::abs(int(y1[i]) - int(y2[i])));
        }
        QVERIFY2(maxY <= 1, qPrintable(QStringLiteral("luma differs by %1").arg(maxY)));
        // Chroma: compare the mean (swscale's chroma filter differs slightly per sample).
        double du = 0;
        double dv = 0;
        for (size_t i = 0; i < u1.size(); ++i) {
            du += int(u1[i]) - int(u2[i]);
            dv += int(v1[i]) - int(v2[i]);
        }
        QVERIFY(std::abs(du / double(u1.size())) < 1.0);
        QVERIFY(std::abs(dv / double(v1.size())) < 1.0);

        // Speed of the hot path (one core), printed for the readme.
        QImage big(1920, 1080, QImage::Format_ARGB32_Premultiplied);
        big.fill(QColor(30, 120, 200));
        std::vector<uint8_t> by(1920 * 1080), bu(1920 * 540);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 20; ++i) {
            exporter::rgb32ToYuv420(big.constBits(), int(big.bytesPerLine()), 1920, 0, 1080,
                {by.data(), 1920, bu.data(), 1920, nullptr, 0, true});
        }
        qInfo("rgb32ToYuv420 1080p NV12: %.2f ms per frame (one thread)",
            double(timer.nsecsElapsed()) / 1e6 / 20);
    }

    void presetsLoad()
    {
        const QList<ExportPreset>& builtins = ExportPresets::builtinPresets();
        QVERIFY(builtins.size() >= 20);
        QStringList ids;
        for (const ExportPreset& p : builtins) {
            ids << p.id;
            QString error;
            ExportSettings s = p.settings;
            s.outputPath = QStringLiteral("/tmp/out.") + s.containerInfo()->extension;
            QVERIFY2(s.validate(&error), qPrintable(p.id + QLatin1String(": ") + error));
        }
        for (const char* required :
            {"youtube_1080p", "youtube_4k", "vimeo_1080p", "master_prores_hq", "archive_dnxhr_hq",
                "web_webm", "audio_wav", "png_sequence", "gif"}) {
            QVERIFY2(ids.contains(QLatin1String(required)), required);
        }
    }

    void userPresets()
    {
        const QString file = m_dir.filePath(QStringLiteral("user_presets.json"));
        QString id;
        {
            ExportPresets presets(file);
            ExportSettings s;
            s.container = QStringLiteral("mkv");
            s.video.codec = QStringLiteral("hevc");
            s.video.quality = 30;
            s.outputPath = QStringLiteral("/never/saved.mkv");
            id = presets.saveUserPreset(QStringLiteral("My HEVC"), s);
            QCOMPARE(id, QStringLiteral("user:My HEVC"));
        }
        ExportPresets reloaded(file);
        const ExportPreset* p = reloaded.find(id);
        QVERIFY(p);
        QVERIFY(!p->builtin);
        QCOMPARE(p->settings.video.codec, QStringLiteral("hevc"));
        QCOMPARE(p->settings.video.quality, 30);
        QVERIFY(p->settings.outputPath.isEmpty());
        QVERIFY(!reloaded.removeUserPreset(QStringLiteral("youtube_1080p")));
        QVERIFY(reloaded.removeUserPreset(id));
        QVERIFY(!ExportPresets(file).find(id));
    }

    void exportPreset_data()
    {
        QTest::addColumn<QString>("presetId");
        for (const ExportPreset& p : ExportPresets::builtinPresets()) {
            QTest::newRow(qPrintable(p.id)) << p.id;
        }
    }

    void exportPreset()
    {
        if (!m_skip.isEmpty()) {
            QSKIP(qPrintable(m_skip));
        }
        QFETCH(QString, presetId);
        ExportSettings s;
        QVERIFY(ExportPresets().apply(presetId, s));
        // Reduced size for speed (GIF keeps its proportional width).
        if (s.video.width > 0 && s.video.height > 0) {
            s.video.width = kProjectWidth;
            s.video.height = kProjectHeight;
        } else if (s.video.width > 0) {
            s.video.width = 160;
        }
        const ContainerInfo* container = s.containerInfo();
        s.outputPath = m_dir.filePath(presetId + QLatin1Char('.') + container->extension);
        // Every speed setting is exercised by some preset; "fast" keeps AV1/HEVC quick here.
        s.video.speed = QStringLiteral("fast");
        const ExportResult result = render(s);
        QVERIFY2(result.ok, qPrintable(result.error));
        qInfo("%s: %lld frames, %.1f fps, encoder %s", qPrintable(presetId),
            static_cast<long long>(result.frames), result.averageFps(),
            qPrintable(result.videoEncoder.isEmpty() ? QStringLiteral("-") : result.videoEncoder));
        const QSize size = s.outputSize(m_project->timeline()->data());

        switch (s.kind()) {
        case OutputKind::Still: {
            QCOMPARE(result.files.size(), 1);
            const QImage image(result.files.first());
            QCOMPARE(image.size(), size);
            QVERIFY2(meanLuma(image) > 10, "frame grab is black");
            return;
        }
        case OutputKind::ImageSequence: {
            QCOMPARE(result.files.size(), int(kProjectFrames));
            for (const QString& file : result.files) {
                QVERIFY2(QFileInfo::exists(file), qPrintable(file));
            }
            const media::MediaInfo info = media::probe(result.files[20]);
            QVERIFY2(info.valid, qPrintable(info.error));
            QCOMPARE(QSize(info.width, info.height), size);
            QVERIFY2(meanLuma(decodeAt(result.files[20], 0)) > 10, "sequence frame is black");
            // The sequence imports as one clip again.
            const media::MediaInfo seq
                = media::probe(result.outputPath + QStringLiteral("#fps=25"));
            QVERIFY2(seq.valid && seq.isImageSequence, qPrintable(seq.error));
            QVERIFY(std::abs(seq.durationUs - kProjectUs) <= kFrameUs);
            return;
        }
        default:
            break;
        }

        QCOMPARE(result.files.size(), 1);
        const media::MediaInfo info = media::probe(result.outputPath);
        QVERIFY2(info.valid, qPrintable(info.error));
        QVERIFY2(std::abs(info.durationUs - kProjectUs) <= kFrameUs,
            qPrintable(QStringLiteral("duration %1 us").arg(info.durationUs)));
        if (s.hasVideo()) {
            QVERIFY(info.hasVideo);
            QCOMPARE(info.videoCodec, probedVideoCodec(s.video.codec));
            QCOMPARE(QSize(info.width, info.height), size);
            QVERIFY2(info.frameRate == (Rational {25, 1}) || s.kind() == OutputKind::Gif,
                qPrintable(
                    QStringLiteral("%1/%2 fps").arg(info.frameRate.num).arg(info.frameRate.den)));
            // Frame 35: the second clip (after the dissolve) — must show picture, not black.
            const QImage frame = decodeAt(result.outputPath, 35 * kFrameUs + kFrameUs / 2);
            QVERIFY2(meanLuma(frame) > 10, "decoded frame is black");
            if (s.keepsAlpha()) {
                QVERIFY(info.hasAlpha);
            }
        } else {
            QVERIFY(!info.hasVideo);
        }
        if (s.hasAudio()) {
            QVERIFY(info.hasAudio);
            QCOMPARE(info.audioCodec, probedAudioCodec(s.audio.codec));
            QCOMPARE(info.audioSampleRate, s.audio.sampleRate);
            QVERIFY2(audioRms(result.outputPath, 48000) > 0.05, "audio is silent");
        }
    }

    void hardwareEncoders()
    {
        if (!m_skip.isEmpty()) {
            QSKIP(qPrintable(m_skip));
        }
        int tested = 0;
        for (const HwEncoderStatus& hw : EncoderCatalog::hardwareEncoders()) {
            qInfo("%-12s %s %s", qPrintable(hw.encoderName),
                hw.available ? "available" : "unavailable",
                qPrintable(hw.available ? hw.device : hw.error));
            if (!hw.available) {
                continue;
            }
            ExportSettings s;
            s.container = hw.codecId == QLatin1String("vp9") ? QStringLiteral("webm")
                                                             : QStringLiteral("mp4");
            s.video.codec = hw.codecId;
            s.video.encoder = hw.backend;
            s.normalize();
            s.outputPath
                = m_dir.filePath(hw.encoderName + QLatin1Char('.') + s.containerInfo()->extension);
            const ExportResult result = render(s);
            QVERIFY2(result.ok, qPrintable(hw.encoderName + QLatin1String(": ") + result.error));
            QCOMPARE(result.videoEncoder, hw.encoderName);
            const media::MediaInfo info = media::probe(result.outputPath);
            QVERIFY(info.valid);
            QCOMPARE(info.videoCodec, hw.codecId);
            QVERIFY(std::abs(info.durationUs - kProjectUs) <= kFrameUs);
            QVERIFY2(
                meanLuma(decodeAt(result.outputPath, 35 * kFrameUs + kFrameUs / 2)) > 10, "black");
            ++tested;
        }
        if (tested == 0) {
            QSKIP("no hardware encoder available on this machine");
        }
    }

    void avSync_data()
    {
        QTest::addColumn<QString>("presetId");
        QTest::newRow("mp4 h264+aac") << QStringLiteral("youtube_1080p");
        QTest::newRow("webm vp9+opus") << QStringLiteral("web_webm");
        QTest::newRow("mov prores+pcm") << QStringLiteral("master_prores_hq");
        QTest::newRow("mkv h264+flac") << QStringLiteral("mkv_h264_flac");
    }

    void avSync()
    {
        if (!m_skip.isEmpty()) {
            QSKIP(qPrintable(m_skip));
        }
        QFETCH(QString, presetId);
        // Black picture with one white frame at kFlash; audio silent except an impulse at the
        // first sample of that frame. After encoding, both must still coincide within 1 frame.
        constexpr int64_t kFlash = 37;
        constexpr int64_t kFrames = 60;
        const QString wav = m_dir.filePath(QStringLiteral("impulse.wav"));
        const int impulse = int(framesToSamples48k(kFlash, {25, 1}));
        QVERIFY(writeWav(
            wav, 48000 * 3, [&](int i) { return (i >= impulse && i < impulse + 48) ? 0.9 : 0.0; }));
        Project project;
        const ImportResult imported = project.importMedia({wav});
        QCOMPARE(imported.importedIds.size(), 1);
        project.beginEdit(QStringLiteral("sync"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = kProjectWidth;
        tl.height = kProjectHeight;
        tl.fps = {25, 1};
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, kFrames));
        addClip(tl, TrackType::Video, 1, solidClip(tl, Qt::white, kFlash, 1));
        addClip(tl, TrackType::Audio, 0,
            mediaClip(tl, imported.importedIds[0], 0, kFrames, 0, 3'000'000));
        project.endEdit();

        ExportSettings s;
        QVERIFY(ExportPresets().apply(presetId, s));
        s.video.width = kProjectWidth;
        s.video.height = kProjectHeight;
        s.outputPath = m_dir.filePath(
            QStringLiteral("sync_") + presetId + QLatin1Char('.') + s.containerInfo()->extension);
        ExportJob job(
            s, project.timeline()->snapshot(), makeMediaPathMap(project.mediaPool()->state()));
        job.setCompositor(m_compositor.get());
        const ExportResult result = job.run();
        QVERIFY2(result.ok, qPrintable(result.error));

        // Brightest decoded frame = the flash.
        media::VideoDecoder decoder;
        QVERIFY(decoder.open(result.outputPath));
        int64_t flashFrame = -1;
        double best = 0;
        for (int64_t f = 0; f < kFrames; ++f) {
            const double luma
                = meanLuma(decoder.frameAt(f * kFrameUs + kFrameUs / 2, QSize(64, 36)));
            if (luma > best) {
                best = luma;
                flashFrame = f;
            }
        }
        QVERIFY2(best > 128, "flash frame not found");
        // Loudest decoded sample = the impulse.
        media::AudioDecoder audio;
        QVERIFY(audio.open(result.outputPath));
        std::vector<float> samples(size_t(48000 * 2) * 2);
        audio.read(0, samples.data(), 48000 * 2);
        int64_t peak = 0;
        float peakValue = 0;
        for (size_t i = 0; i < samples.size(); i += 2) {
            if (std::abs(samples[i]) > peakValue) {
                peakValue = std::abs(samples[i]);
                peak = int64_t(i / 2);
            }
        }
        QVERIFY2(peakValue > 0.3f, "impulse not found");
        const double videoUs = double(flashFrame) * kFrameUs;
        const double audioUs = double(peak) * 1e6 / 48000.0;
        qInfo("%s: flash at %.1f ms, impulse at %.1f ms (offset %.1f ms)", qPrintable(presetId),
            videoUs / 1000, audioUs / 1000, (audioUs - videoUs) / 1000);
        QCOMPARE(flashFrame, kFlash);
        QVERIFY2(std::abs(audioUs - videoUs) <= double(kFrameUs),
            qPrintable(QStringLiteral("A/V offset %1 ms").arg((audioUs - videoUs) / 1000)));
    }

    void cancelRemovesPartialFile_data()
    {
        QTest::addColumn<QString>("presetId");
        QTest::newRow("mov") << QStringLiteral("master_prores_hq");
        QTest::newRow("png sequence") << QStringLiteral("png_sequence");
    }

    void cancelRemovesPartialFile()
    {
        if (!m_skip.isEmpty()) {
            QSKIP(qPrintable(m_skip));
        }
        QFETCH(QString, presetId);
        ExportSettings s;
        QVERIFY(ExportPresets().apply(presetId, s));
        s.outputPath = m_dir.filePath(
            QStringLiteral("cancel_") + presetId + QLatin1Char('.') + s.containerInfo()->extension);
        ExportJob job(s, m_project->timeline()->snapshot(),
            makeMediaPathMap(m_project->mediaPool()->state()));
        job.setCompositor(m_compositor.get());
        bool sawFile = false;
        const ExportResult result = job.run([&](const ExportProgress& p) {
            if (p.framesDone == 10) {
                sawFile = QFileInfo::exists(s.kind() == OutputKind::ImageSequence
                        ? QString::asprintf(qPrintable(sequencePattern(s.outputPath)), 0)
                        : s.outputPath);
                job.cancel();
            }
        });
        QVERIFY(sawFile);
        QVERIFY(!result.ok);
        QVERIFY(result.cancelled);
        QVERIFY(!QFileInfo::exists(s.outputPath));
        const QStringList left
            = QDir(m_dir.path()).entryList({QStringLiteral("cancel_*")}, QDir::Files);
        QVERIFY2(left.isEmpty(), qPrintable(left.join(QLatin1Char(' '))));
    }

    void errorsAreReported()
    {
        if (!m_skip.isEmpty()) {
            QSKIP(qPrintable(m_skip));
        }
        ExportSettings s;
        s.outputPath = QStringLiteral("/proc/kinoklip-cannot-write/out.mp4");
        const ExportResult r = render(s);
        QVERIFY(!r.ok);
        QVERIFY(!r.cancelled);
        QVERIFY(!r.error.isEmpty());

        ExportSettings empty;
        empty.outputPath = m_dir.filePath(QStringLiteral("empty.mp4"));
        empty.range = RangeMode::Custom;
        empty.rangeIn = 10;
        empty.rangeOut = 10;
        QVERIFY(!render(empty).ok);
    }

    void customRange()
    {
        if (!m_skip.isEmpty()) {
            QSKIP(qPrintable(m_skip));
        }
        ExportSettings s;
        s.range = RangeMode::Custom;
        s.rangeIn = 10;
        s.rangeOut = 35;
        s.outputPath = m_dir.filePath(QStringLiteral("range.mp4"));
        const ExportResult r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.frames, int64_t(25));
        const media::MediaInfo info = media::probe(r.outputPath);
        QVERIFY(std::abs(info.durationUs - 25 * kFrameUs) <= kFrameUs);
    }

    /// Render Settings "Key frames: every N" and "Frame reordering" reach the encoder.
    void keyframesAndReordering()
    {
        if (!m_skip.isEmpty()) {
            QSKIP(qPrintable(m_skip));
        }
        ExportSettings s;
        s.video.encoder = EncoderBackend::Software; // deterministic GOP handling
        s.video.keyframeInterval = 5;
        s.video.frameReordering = false;
        s.audio.enabled = false;
        s.range = RangeMode::Custom;
        s.rangeIn = 0;
        s.rangeOut = 25;
        s.outputPath = m_dir.filePath(QStringLiteral("gop.mp4"));
        const ExportResult r = render(s);
        QVERIFY2(r.ok, qPrintable(r.error));

        AVFormatContext* format = nullptr;
        QCOMPARE(
            avformat_open_input(&format, r.outputPath.toUtf8().constData(), nullptr, nullptr), 0);
        QVERIFY(avformat_find_stream_info(format, nullptr) >= 0);
        AVPacket* packet = av_packet_alloc();
        int frames = 0;
        int lastKey = -1;
        int longestGop = 0;
        bool reordered = false;
        // Only the video stream (the file also has a timecode track, spec 25.12).
        const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index != video) {
                av_packet_unref(packet);
                continue;
            }
            if (packet->flags & AV_PKT_FLAG_KEY) {
                longestGop = std::max(longestGop, frames - lastKey);
                lastKey = frames;
            }
            ++frames;
            reordered |= packet->pts != packet->dts; // B-frames need pts != dts
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        avformat_close_input(&format);
        longestGop = std::max(longestGop, frames - lastKey);
        QCOMPARE(frames, 25);
        // A key frame at least every 5 frames (scene cuts may add more); the default (2 s)
        // would give one key frame for the whole second.
        QVERIFY2(longestGop <= 5, qPrintable(QString::number(longestGop)));
        QVERIFY(!reordered);
    }

    void renderQueue()
    {
        if (!m_skip.isEmpty()) {
            QSKIP(qPrintable(m_skip));
        }
        RenderQueue queue;
        QSignalSpy finished(&queue, &RenderQueue::jobFinished);
        ExportSettings a;
        a.outputPath = m_dir.filePath(QStringLiteral("queue_a.mp4"));
        ExportSettings b;
        QVERIFY(ExportPresets().apply(QStringLiteral("audio_wav"), b));
        b.outputPath = m_dir.filePath(QStringLiteral("queue_b.wav"));
        auto media = makeMediaPathMap(m_project->mediaPool()->state());
        const quint64 idA
            = queue.addJob(QStringLiteral("A"), a, m_project->timeline()->snapshot(), media);
        const quint64 idB
            = queue.addJob(QStringLiteral("B"), b, m_project->timeline()->snapshot(), media);
        // Editing after queueing must not change the queued render (snapshot semantics).
        m_project->beginEdit(QStringLiteral("edit"));
        m_project->timeline()->mutableData().videoTracks[0].clips.clear();
        m_project->endEdit();
        queue.startAll();
        QVERIFY(queue.isRunning());
        QVERIFY(queue.waitForIdle(120000));
        m_project->undo();
        QCOMPARE(finished.count(), 2);
        QCOMPARE(queue.job(idA)->status, RenderQueue::Status::Done);
        QCOMPARE(queue.job(idB)->status, RenderQueue::Status::Done);
        QVERIFY(meanLuma(decodeAt(a.outputPath, 35 * kFrameUs)) > 10);
        queue.clearFinished();
        QVERIFY(queue.jobs().isEmpty());
    }

    void commandLineRender()
    {
        if (!m_skip.isEmpty()) {
            QSKIP(qPrintable(m_skip));
        }
        const QString app = QCoreApplication::applicationDirPath() + QStringLiteral("/kinoklip");
        if (!QFileInfo::exists(app)) {
            QSKIP("kinoklip executable not built next to the test");
        }
        const QString projectPath = m_dir.filePath(QStringLiteral("cli.kkproj"));
        QString error;
        QVERIFY2(m_project->save(projectPath, &error), qPrintable(error));
        for (const QString& name : {QStringLiteral("cli.mp4"), QStringLiteral("cli.wav")}) {
            const QString out = m_dir.filePath(name);
            QProcess process;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            process.setProcessEnvironment(env);
            process.start(app, {QStringLiteral("--render"), projectPath, out});
            QVERIFY(process.waitForFinished(120000));
            const QByteArray output
                = process.readAllStandardOutput() + process.readAllStandardError();
            QVERIFY2(process.exitCode() == 0, output.constData());
            QVERIFY2(output.contains("render:"), output.constData());
            const media::MediaInfo info = media::probe(out);
            QVERIFY2(info.valid, qPrintable(info.error));
            QVERIFY(std::abs(info.durationUs - kProjectUs) <= kFrameUs);
            QVERIFY(info.hasAudio);
        }
        // Preset option + error path.
        QProcess bad;
        bad.start(app,
            {QStringLiteral("--render"), projectPath, m_dir.filePath(QStringLiteral("x.mp4")),
                QStringLiteral("--preset"), QStringLiteral("no_such_preset")});
        QVERIFY(bad.waitForFinished(60000));
        QCOMPARE(bad.exitCode(), 1);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    TestExport test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_export.moc"
