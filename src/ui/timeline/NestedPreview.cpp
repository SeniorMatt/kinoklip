// SPDX-License-Identifier: GPL-3.0-or-later
// NestedPreview: background thumbnails / waveforms of nested timelines.
#include "ui/timeline/NestedPreview.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "render/AudioMixer.h"
#include "render/Compositor.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <future>

namespace kk {

namespace {

const QString kScheme = QStringLiteral("kk-nested:");
constexpr int kPeaksPerSecond = 100; ///< same resolution as media::MediaCache waveforms
constexpr int kPollMs = 30; ///< thumbnail future polling interval
/// Thumbnails are rendered at most this many per second of nested time (zoomed-in tiles
/// share them), which bounds the work per timeline.
constexpr double kThumbsPerSecond = 4.0;

} // namespace

QString nestedPreviewPath(quint64 uid)
{
    return kScheme + QString::number(uid);
}

quint64 nestedPreviewUid(const QString& path)
{
    if (!path.startsWith(kScheme))
        return 0;
    return path.mid(kScheme.size()).toULongLong();
}

NestedPreview::NestedPreview(Project* project, QObject* parent)
    : QObject(parent)
    , m_project(project)
{
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &NestedPreview::poll);
}

NestedPreview::~NestedPreview()
{
    m_cancel->store(true); // waveform workers stop at their next block
    for (PendingThumb& p : m_pending) {
        if (p.future.valid())
            p.future.wait(); // the compositor must outlive its requests
    }
}

NestedPreview::Entry& NestedPreview::entry(quint64 uid)
{
    Entry& e = m_entries[uid];
    // Multicam angle keys (multicamViewKey()) depend on the multicam timeline itself.
    std::shared_ptr<const TimelineData> current = m_project->timelineData(uid & kTimelineUidMask);
    if (current != e.version) {
        // The nested timeline changed: everything made from the old version is stale.
        e.version = std::move(current);
        e.thumbs.clear();
        e.peaks.reset();
        e.pendingPeaks = {};
    }
    return e;
}

QImage NestedPreview::thumbnail(quint64 uid, int64_t timeUs, int height)
{
    Entry& e = entry(uid);
    if (!e.version || height <= 0)
        return {};
    const Rational fps = e.version->fps;
    const int64_t step = std::max<int64_t>(1, int64_t(fps.toDouble() / kThumbsPerSecond));
    const int64_t frame = std::max<int64_t>(0, usToFrames(timeUs, fps)) / step * step;
    const qint64 key = qint64(frame) * 4096 + std::min(height, 4095);
    if (const auto it = e.thumbs.constFind(key); it != e.thumbs.constEnd())
        return *it;
    for (const PendingThumb& p : m_pending) {
        if (p.uid == uid && p.key == key)
            return {};
    }
    if (int(m_pending.size()) >= kMaxPending)
        return {}; // asked again on the next repaint
    if (!m_compositor)
        m_compositor = std::make_unique<Compositor>();
    if (m_pathsRevision != m_project->mediaPool()->revision()) {
        m_paths = makeMediaPathMap(m_project->mediaPool()->state());
        m_pathsRevision = m_project->mediaPool()->revision();
    }
    if (e.thumbs.size() > kMaxThumbsPerTimeline)
        e.thumbs.clear();
    const int width
        = std::max(1, int(std::lround(height * double(e.version->width) / e.version->height)));
    RenderRequest rq;
    rq.timeline = m_project->renderSnapshot(uid);
    rq.media = m_paths;
    rq.frame = frame;
    rq.outSize = QSize(width, height);
    rq.outputs = OutputImage;
    m_pending.push_back({uid, key, e.version.get(), m_compositor->renderAsync(std::move(rq))});
    m_poll.start();
    return {};
}

void NestedPreview::poll()
{
    bool any = false;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        const RenderedFramePtr frame = it->future.get();
        Entry& e = m_entries[it->uid];
        // Composite over black like the viewer, so transparent areas read as empty.
        if (frame && frame->ok && e.version.get() == it->version) {
            QImage image(frame->image.size(), QImage::Format_RGB32);
            image.fill(Qt::black);
            {
                QPainter p(&image);
                p.drawImage(0, 0, frame->image);
            }
            e.thumbs.insert(it->key, image);
            any = true;
        }
        it = m_pending.erase(it);
    }
    for (Entry& e : m_entries) {
        if (!e.peaks && e.pendingPeaks.valid()
            && e.pendingPeaks.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            e.peaks = e.pendingPeaks.get();
            e.pendingPeaks = {};
            any = true;
        }
    }
    const bool waiting = !m_pending.empty()
        || std::any_of(m_entries.cbegin(), m_entries.cend(),
            [](const Entry& e) { return e.pendingPeaks.valid(); });
    if (!waiting)
        m_poll.stop();
    if (any)
        emit ready();
}

bool NestedPreview::waveform(
    quint64 uid, double startUs, double usPerColumn, int columns, float* peaks)
{
    Entry& e = entry(uid);
    if (!e.version)
        return false;
    if (!e.peaks) {
        if (!e.pendingPeaks.valid()) {
            // Mix the whole nested timeline once on a worker thread (own mixer: not
            // thread-safe), keeping 10 ms peaks. Rare (once per change of the timeline), so
            // a plain std::async thread is fine.
            std::shared_ptr<const TimelineData> shot = m_project->renderSnapshot(uid);
            std::shared_ptr<const MediaPathMap> paths
                = makeMediaPathMap(m_project->mediaPool()->state());
            e.pendingPeaks = std::async(std::launch::async, [shot, paths, cancel = m_cancel]() {
                auto result = std::make_shared<Peaks>();
                const int64_t total = framesToSamples48k(shot->duration(), shot->fps);
                constexpr int kBlock = AudioMixer::kSampleRate / kPeaksPerSecond;
                result->peak.reserve(size_t(total / kBlock + 1));
                AudioMixer mixer;
                mixer.setMediaPaths(paths);
                std::vector<float> buffer(size_t(kBlock) * AudioMixer::kChannels);
                for (int64_t s = 0; s < total && !cancel->load(); s += kBlock) {
                    mixer.mix(*shot, s, kBlock, buffer.data());
                    float peak = 0.0f;
                    for (const float v : buffer)
                        peak = std::max(peak, std::abs(v));
                    result->peak.push_back(std::min(1.0f, peak));
                }
                return std::shared_ptr<const Peaks>(std::move(result));
            }).share();
            m_poll.start();
        }
        return false;
    }
    const std::vector<float>& p = e.peaks->peak;
    const int n = int(p.size());
    const double perUs = kPeaksPerSecond / 1e6;
    for (int c = 0; c < columns; ++c) {
        const double t0 = startUs + c * usPerColumn;
        int i0 = int(std::floor(t0 * perUs));
        int i1 = int(std::floor((t0 + usPerColumn) * perUs));
        if (i1 <= i0)
            i1 = i0 + 1;
        i0 = std::clamp(i0, 0, n);
        i1 = std::clamp(i1, 0, n);
        float peak = 0.0f;
        for (int i = i0; i < i1; ++i)
            peak = std::max(peak, p[size_t(i)]);
        peaks[c] = peak;
    }
    return true;
}

ThumbnailProvider NestedPreview::wrap(ThumbnailProvider media)
{
    return [this, media = std::move(media)](const QString& path, int64_t timeUs, int height) {
        if (const quint64 uid = nestedPreviewUid(path))
            return thumbnail(uid, timeUs, height);
        return media ? media(path, timeUs, height) : QImage();
    };
}

WaveformProvider NestedPreview::wrap(WaveformProvider media)
{
    return [this, media = std::move(media)](
               const QString& path, double startUs, double usPerColumn, int columns, float* peaks) {
        if (const quint64 uid = nestedPreviewUid(path))
            return waveform(uid, startUs, usPerColumn, columns, peaks);
        return media ? media(path, startUs, usPerColumn, columns, peaks) : false;
    };
}

} // namespace kk
