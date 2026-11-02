// SPDX-License-Identifier: GPL-3.0-or-later
// NestedPreview — thumbnails and waveforms of compound clips / nested timelines for the
// timeline view: rendered from the nested timeline (Compositor / AudioMixer) in the
// background and cached until that timeline changes.
#pragma once

#include "ui/timeline/TimelineMedia.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <future>
#include <memory>
#include <vector>

namespace kk {

class Compositor;
class Project;
struct RenderedFrame;
struct TimelineData;

/// Pseudo media path the painter uses for the Nested clips of timeline @p uid, so the
/// ordinary ThumbnailProvider / WaveformProvider signatures carry them.
[[nodiscard]] QString nestedPreviewPath(quint64 uid);
/// The timeline uid of a nestedPreviewPath(), or 0 for an ordinary media path.
[[nodiscard]] quint64 nestedPreviewUid(const QString& path);

/// Cache of nested-timeline previews.
///
/// * Thumbnails: frames rendered by a private Compositor (created on first use) at the
///   requested height, quantised to a few hundred per timeline, at most kMaxPending in flight;
///   a QTimer polls the futures (never blocks the GUI thread).
/// * Waveforms: the nested timeline's mix as 100 min/max peaks per second, computed once per
///   version of the timeline on a worker thread.
/// Entries of a timeline are dropped as soon as its data changes (pointer of
/// Project::timelineData()). ready() asks the view to repaint.
///
/// Thread-safety: GUI thread only (worker results come back through futures).
class NestedPreview : public QObject
{
    Q_OBJECT
public:
    explicit NestedPreview(Project* project, QObject* parent = nullptr);
    ~NestedPreview() override;

    /// Thumbnail of nested timeline @p uid at its time @p timeUs, @p height px high; null
    /// while it is being rendered.
    QImage thumbnail(quint64 uid, int64_t timeUs, int height);
    /// WaveformProvider semantics for nested timeline @p uid (false while computing).
    bool waveform(quint64 uid, double startUs, double usPerColumn, int columns, float* peaks);

    /// @p media extended with nested timelines (paths from nestedPreviewPath()).
    [[nodiscard]] ThumbnailProvider wrap(ThumbnailProvider media);
    /// @p media extended with nested timelines.
    [[nodiscard]] WaveformProvider wrap(WaveformProvider media);

signals:
    /// A thumbnail or waveform became available.
    void ready();

private:
    struct Peaks
    {
        std::vector<float> peak; ///< max |sample| per 10 ms, 0..1
    };
    struct Entry
    {
        std::shared_ptr<const TimelineData> version; ///< data the entries were made from
        QHash<qint64, QImage> thumbs; ///< key: quantised frame * 4096 + height
        std::shared_ptr<const Peaks> peaks;
        std::shared_future<std::shared_ptr<const Peaks>> pendingPeaks;
    };
    struct PendingThumb
    {
        quint64 uid = 0;
        qint64 key = 0;
        const TimelineData* version = nullptr;
        std::future<std::shared_ptr<const RenderedFrame>> future;
    };
    Entry& entry(quint64 uid);
    void poll();

    static constexpr int kMaxPending = 4; ///< thumbnail renders in flight
    static constexpr int kMaxThumbsPerTimeline = 400; ///< cache bound per timeline

    Project* m_project;
    std::unique_ptr<Compositor> m_compositor;
    std::shared_ptr<const QHash<QString, QString>> m_paths; ///< media paths (pool revision)
    quint64 m_pathsRevision = ~quint64(0);
    QHash<quint64, Entry> m_entries;
    std::vector<PendingThumb> m_pending;
    QTimer m_poll;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};

} // namespace kk
