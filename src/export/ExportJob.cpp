// SPDX-License-Identifier: GPL-3.0-or-later
// ExportJob — the render/encode/mux loop.
#include "export/ExportJob.h"

#include "core/TimelineData.h"
#include "export/AudioEncoder.h"
#include "export/EdlExport.h"
#include "export/EncoderCatalog.h"
#include "export/ExportLog.h"
#include "export/MediaWriter.h"
#include "export/SubtitleExport.h"
#include "export/VideoEncoder.h"
#include "render/AudioMixer.h"
#include "render/Compositor.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPainter>

#include <deque>
#include <future>
#include <vector>

namespace kk {

using namespace kk::exporter;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::ExportJob", text);
}

/// Audio is mixed in blocks of at most this many 48 kHz sample frames.
constexpr int kMixBlock = 4096;
/// Audio-only jobs advance this many timeline frames per loop iteration.
constexpr int64_t kAudioOnlyStep = 50;
/// Speed is measured over this window.
constexpr double kSpeedWindowSeconds = 1.0;

/// Sliding-window speed meter (allocation-free: fixed ring of timestamps).
class SpeedMeter
{
public:
    void start() { m_timer.start(); }
    void add(int64_t framesDone)
    {
        const double now = double(m_timer.nsecsElapsed()) / 1e9;
        m_samples[m_head] = {now, framesDone};
        m_head = (m_head + 1) % kSize;
        m_count = std::min(m_count + 1, kSize);
    }
    [[nodiscard]] double elapsed() const { return double(m_timer.nsecsElapsed()) / 1e9; }
    [[nodiscard]] double fps() const
    {
        if (m_count < 2) {
            return 0.0;
        }
        const Sample& newest = m_samples[(m_head + kSize - 1) % kSize];
        // Oldest sample that is still inside the window (at least one step back).
        int back = 1;
        for (; back < m_count - 1; ++back) {
            const Sample& s = m_samples[(m_head + kSize - 1 - back) % kSize];
            if (newest.seconds - s.seconds >= kSpeedWindowSeconds) {
                break;
            }
        }
        const Sample& oldest = m_samples[(m_head + kSize - 1 - back) % kSize];
        const double dt = newest.seconds - oldest.seconds;
        return dt > 0 ? double(newest.frames - oldest.frames) / dt : 0.0;
    }

private:
    struct Sample
    {
        double seconds = 0;
        int64_t frames = 0;
    };
    static constexpr int kSize = 256;
    Sample m_samples[kSize];
    int m_head = 0;
    int m_count = 0;
    QElapsedTimer m_timer;
};

/// Files of an image sequence pattern for frame numbers [0, count).
QStringList sequenceFiles(const QString& pattern, int64_t count)
{
    QStringList files;
    const QByteArray format = pattern.toUtf8();
    for (int64_t i = 0; i < count; ++i) {
        files << QString::asprintf(format.constData(), int(i));
    }
    return files;
}

void removeFiles(const QStringList& files)
{
    for (const QString& file : files) {
        QFile::remove(file);
    }
}

/// Premultiplied ARGB → opaque RGB32 over black (premultiplied colour = colour over black).
QImage overBlack(const QImage& image)
{
    QImage out(image.size(), QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.drawImage(0, 0, image);
    return out;
}

} // namespace

ExportJob::ExportJob(ExportSettings settings, std::shared_ptr<const TimelineData> timeline,
    std::shared_ptr<const MediaPathMap> media)
    : m_settings(std::move(settings))
    , m_timeline(std::move(timeline))
    , m_media(std::move(media))
{ }

ExportJob::~ExportJob() = default;

ExportResult ExportJob::run(const ProgressCallback& progress)
{
    ExportResult result;
    QString error;
    if (!m_timeline) {
        result.error = tr("No timeline to render.");
        return result;
    }
    if (!m_settings.validate(&error)) {
        result.error = error;
        return result;
    }
    if (m_settings.hasVideo() && !m_compositor) {
        result.error = tr("No renderer available.");
        return result;
    }
    const QFileInfo out(m_settings.outputPath);
    if (!QDir().mkpath(out.absolutePath())) {
        result.error = tr("Cannot create the folder %1.").arg(out.absolutePath());
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    result = m_settings.kind() == OutputKind::Still ? runStill() : runStreams(progress);
    result.seconds = double(timer.nsecsElapsed()) / 1e9;
    if (result.ok) {
        qCInfo(lcExport).noquote()
            << QStringLiteral("rendered %1 frames to %2 in %3 s (%4 fps, %5)")
                   .arg(result.frames)
                   .arg(result.outputPath)
                   .arg(result.seconds, 0, 'f', 2)
                   .arg(result.averageFps(), 0, 'f', 1)
                   .arg(result.videoEncoder.isEmpty() ? QStringLiteral("audio only")
                                                      : result.videoEncoder);
    } else if (!result.cancelled) {
        qCWarning(lcExport).noquote() << "render failed:" << result.error;
    }
    return result;
}

ExportResult ExportJob::runStill()
{
    ExportResult result;
    result.outputPath = m_settings.outputPath;
    const auto [first, end] = m_settings.frameRange(*m_timeline);
    Q_UNUSED(end);
    const QSize size = m_settings.outputSize(*m_timeline);
    RenderRequest request {m_timeline, m_media, first, size, Quality::Full, OutputImage};
    request.subtitles = m_settings.burnsInSubtitles();
    request.proxies = m_proxies;
    const RenderedFramePtr frame = m_compositor->renderAsync(std::move(request)).get();
    if (!frame || !frame->ok || frame->image.isNull()) {
        result.error = tr("Rendering frame %1 failed.").arg(first);
        return result;
    }
    const bool jpeg = m_settings.video.codec == QLatin1String("mjpeg");
    const QImage image
        = (jpeg || !m_settings.keepsAlpha()) ? overBlack(frame->image) : frame->image;
    // JPEG quality: map the qscale-like setting (2 best .. 31 worst) onto Qt's 100..0.
    const int quality = jpeg ? std::clamp(100 - (m_settings.video.quality - 2) * 3, 1, 100) : -1;
    if (!image.save(m_settings.outputPath, jpeg ? "JPEG" : "PNG", quality)) {
        result.error = tr("Cannot write %1.").arg(m_settings.outputPath);
        QFile::remove(m_settings.outputPath);
        return result;
    }
    result.ok = true;
    result.frames = 1;
    result.files = {m_settings.outputPath};
    return result;
}

ExportResult ExportJob::runStreams(const ProgressCallback& progress)
{
    ExportResult result;
    const ExportSettings& s = m_settings;
    const TimelineData& tl = *m_timeline;
    const ContainerInfo* container = s.containerInfo();
    const auto [first, end] = s.frameRange(tl);
    const int64_t total = end - first;
    if (total <= 0) {
        result.error = tr("Nothing to render: the render range is empty.");
        return result;
    }
    const bool sequence = s.kind() == OutputKind::ImageSequence;
    result.outputPath = sequence ? sequencePattern(s.outputPath) : s.outputPath;
    const QSize size = s.outputSize(tl);

    MediaWriter writer;
    QString error;
    if (!writer.open(container->muxer, result.outputPath, &error)) {
        result.error = error;
        return result;
    }

    // ---- Video encoder: first candidate that opens at the real size ----
    VideoEncoder video;
    if (s.hasVideo()) {
        const QList<EncoderChoice> choices
            = EncoderCatalog::candidates(s.video.codec, s.video.encoder);
        if (choices.isEmpty()) {
            result.error = tr("No encoder for %1 is available.").arg(s.video.codec);
            return result;
        }
        bool opened = false;
        QStringList failures;
        for (const EncoderChoice& choice : choices) {
            VideoEncoderConfig config;
            config.encoderName = choice.encoderName;
            config.backend = choice.backend;
            config.settings = s.video;
            config.size = size;
            config.fps = tl.fps;
            config.alpha = s.keepsAlpha();
            config.globalHeader = writer.needsGlobalHeader();
            BufferRefPtr device;
            if (choice.backend == EncoderBackend::Vaapi) {
                device.reset(EncoderCatalog::vaapiDevice(choice.encoderName, &config.lowPower));
                config.hwDevice = device.get();
            }
            if (video.open(config, &error)) {
                result.videoEncoder = choice.encoderName;
                result.videoBackend = choice.backend;
                opened = true;
                break;
            }
            qCWarning(lcExport).noquote()
                << "falling back from" << choice.encoderName << ":" << error;
            failures << error;
        }
        if (!opened) {
            result.error
                = tr("Cannot start the video encoder: %1").arg(failures.join(QStringLiteral("; ")));
            return result;
        }
    }
    const int videoStream = s.hasVideo() ? writer.addStream(video.context(), &error) : -1;
    // The record timecode of the first rendered frame (a "tmcd" track in QuickTime / MP4, a
    // tag elsewhere), so the file starts at the timeline's timecode as in Resolve.
    if (videoStream >= 0 && !sequence)
        writer.setStreamMetadata(
            videoStream, "timecode", edl::timecode(first + tl.startTimecode, tl.fps));

    AudioEncoder audio;
    int audioStream = -1;
    if (s.hasAudio()) {
        if (!audio.open(s.audio, writer.needsGlobalHeader(), &error)) {
            result.error = tr("Cannot start the audio encoder: %1").arg(error);
            return result;
        }
        audioStream = writer.addStream(audio.context(), &error);
    }
    if ((s.hasVideo() && videoStream < 0) || (s.hasAudio() && audioStream < 0)) {
        result.error = error;
        return result;
    }

    // ---- Subtitles: one embedded stream per enabled subtitle track with captions ----
    const QVector<SubtitleTrackExport> captionTracks = s.subtitles.enabled
        ? collectSubtitleTracks(tl, first, end)
        : QVector<SubtitleTrackExport> {};
    const QString subtitleCodec = s.embeddedSubtitleCodec();
    std::vector<std::unique_ptr<SubtitleEncoder>> subtitleEncoders;
    std::vector<int> subtitleStreams;
    if (!subtitleCodec.isEmpty() && !sequence) {
        for (const SubtitleTrackExport& track : captionTracks) {
            auto encoder = std::make_unique<SubtitleEncoder>();
            if (!encoder->open(subtitleCodec, size.width(), size.height(),
                    writer.needsGlobalHeader(), &error)) {
                result.error = error;
                return result;
            }
            const int stream = writer.addStream(encoder->context(), &error);
            if (stream < 0) {
                result.error = error;
                return result;
            }
            writer.setStreamMetadata(
                stream, "language", containerLanguage(track.language, s.container));
            writer.setStreamMetadata(stream, "title", track.name);
            subtitleStreams.push_back(stream);
            subtitleEncoders.push_back(std::move(encoder));
        }
    }
    if (!writer.writeHeader(container->faststart, 0, &error)) {
        result.error = error;
        writer.abort();
        QFile::remove(result.outputPath);
        return result;
    }
    // Captions are tiny: all of them are queued right away; the interleaving writer holds
    // them until the picture and sound reach their time.
    for (size_t i = 0; i < subtitleEncoders.size(); ++i) {
        const int stream = subtitleStreams[i];
        const AVRational tb = subtitleEncoders[i]->context()->time_base;
        for (const SubtitleCue& cue : captionTracks[int(i)].cues) {
            const bool ok = subtitleEncoders[i]->encode(
                cue, [&](AVPacket* p) { return writer.write(p, stream, tb, &error); }, &error);
            if (!ok) {
                result.error = tr("Subtitle encoding failed: %1").arg(error);
                writer.abort();
                QFile::remove(result.outputPath);
                return result;
            }
        }
    }

    // From here on every failure path must delete what was written.
    auto failWith = [&](const QString& message, bool cancelled) {
        writer.abort();
        removeFiles(
            sequence ? sequenceFiles(result.outputPath, total) : QStringList {result.outputPath});
        result.ok = false;
        result.cancelled = cancelled;
        result.error = cancelled ? tr("Cancelled.") : message;
        return result;
    };

    QString writeError;
    const AVRational videoTb = s.hasVideo() ? video.context()->time_base : AVRational {1, 1};
    const AVRational audioTb = s.hasAudio() ? audio.context()->time_base : AVRational {1, 1};
    const PacketSink videoSink
        = [&](AVPacket* p) { return writer.write(p, videoStream, videoTb, &writeError); };
    const PacketSink audioSink
        = [&](AVPacket* p) { return writer.write(p, audioStream, audioTb, &writeError); };

    AudioMixer mixer;
    mixer.setMediaPaths(m_media);
    std::vector<float> mixBuffer(size_t(kMixBlock) * AudioMixer::kChannels);
    const int64_t audioBase = framesToSamples48k(first, tl.fps);
    int64_t audioDone = 0; ///< 48 kHz sample frames mixed so far (relative to audioBase)
    // Mixes and encodes the audio up to (exclusive) range-relative timeline frame @p frameEnd.
    auto encodeAudioUntil = [&](int64_t frameEnd) {
        const int64_t target = framesToSamples48k(first + frameEnd, tl.fps) - audioBase;
        while (audioDone < target) {
            const int n = int(std::min<int64_t>(kMixBlock, target - audioDone));
            mixer.mix(tl, audioBase + audioDone, n, mixBuffer.data());
            if (!audio.encode(mixBuffer.data(), n, audioSink, &error)) {
                return false;
            }
            audioDone += n;
        }
        return true;
    };

    SpeedMeter meter;
    meter.start();
    ExportProgress state;
    state.totalFrames = total;
    std::deque<std::future<RenderedFramePtr>> inFlight;
    int64_t requested = 0;
    int64_t done = 0;
    QImage converted; // reused when the compositor delivers another format
    while (done < total) {
        if (isCancelled()) {
            return failWith({}, true);
        }
        int64_t step = 1;
        if (s.hasVideo()) {
            while (int64_t(inFlight.size()) < kFramesInFlight && requested < total) {
                RenderRequest request {
                    m_timeline, m_media, first + requested, size, Quality::Full, OutputImage};
                request.subtitles = s.burnsInSubtitles();
                request.proxies = m_proxies; // nullptr unless "Use proxy media" was chosen
                inFlight.push_back(m_compositor->renderAsync(std::move(request)));
                ++requested;
            }
            const RenderedFramePtr frame = inFlight.front().get();
            inFlight.pop_front();
            if (!frame || !frame->ok || frame->image.isNull()) {
                return failWith(tr("Rendering frame %1 failed.").arg(first + done), false);
            }
            const QImage* image = &frame->image;
            if (image->format() != QImage::Format_ARGB32_Premultiplied || image->size() != size) {
                converted = frame->image.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                                .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                image = &converted;
            }
            if (!video.encode(*image, done, videoSink, &error)) {
                return failWith(
                    writeError.isEmpty() ? tr("Video encoding failed: %1").arg(error) : writeError,
                    false);
            }
        } else {
            step = std::min(kAudioOnlyStep, total - done);
        }
        if (s.hasAudio() && !encodeAudioUntil(done + step)) {
            return failWith(
                writeError.isEmpty() ? tr("Audio encoding failed: %1").arg(error) : writeError,
                false);
        }
        done += step;
        meter.add(done);
        if (progress) {
            state.framesDone = done;
            state.elapsedSeconds = meter.elapsed();
            state.fps = meter.fps();
            state.etaSeconds = state.fps > 0 ? double(total - done) / state.fps : -1.0;
            progress(state);
        }
    }
    if (s.hasVideo() && !video.flush(videoSink, &error)) {
        return failWith(
            writeError.isEmpty() ? tr("Video encoding failed: %1").arg(error) : writeError, false);
    }
    if (s.hasAudio() && !audio.flush(audioSink, &error)) {
        return failWith(
            writeError.isEmpty() ? tr("Audio encoding failed: %1").arg(error) : writeError, false);
    }
    if (isCancelled()) {
        return failWith({}, true);
    }
    if (!writer.finish(&error)) {
        removeFiles(
            sequence ? sequenceFiles(result.outputPath, total) : QStringList {result.outputPath});
        result.error = error;
        return result;
    }
    if (s.hasVideo()) {
        qCDebug(lcExport) << "pixel conversion" << video.convertMs() / double(total)
                          << "ms/frame, encoder" << video.encodeMs() / double(total) << "ms/frame";
    }
    result.ok = true;
    result.frames = total;
    result.files
        = sequence ? sequenceFiles(result.outputPath, total) : QStringList {result.outputPath};
    // Separate subtitle files next to the render (Resolve: "As a separate file").
    if (s.subtitles.enabled && s.subtitles.mode == SubtitleExportMode::Sidecar
        && !writeSidecars(
            s.outputPath, captionTracks, s.subtitles.sidecarFormat, &result.files, &error)) {
        result.ok = false;
        result.error = error;
    }
    return result;
}

} // namespace kk
