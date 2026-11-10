// SPDX-License-Identifier: GPL-3.0-or-later
// MediaHoverPreview — Resolve's "Live Media Preview": while the pointer moves over a clip's
// thumbnail in the Media Pool, the source viewer shows the frame at the pointer's horizontal
// place (left edge = first frame, right edge = end of the clip); leaving restores the viewer.
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

#include <cstdint>

namespace kk {

class PlaybackEngine;
class ViewerPanel;

/// Drives the hover preview of one source viewer: hover() names a clip file and a place along
/// it, stop() ends the preview.
///
/// - The frame comes from media::MediaCache::frame() (its decoder pool, off the GUI thread). One
///   request is in flight at a time; moves meanwhile only change the wanted time, and when the
///   frame arrives the newest wanted time is requested (latest wins). A frame already cached is
///   shown at once.
/// - The picture is drawn with ViewerPanel::showTemporaryFrame(); the viewer's clip, position
///   and marks are never touched, so stop() restores exactly what it showed before.
/// - A playing viewer is left alone: hover() does nothing, and a preview shown when playback
///   starts ends. So does loading another clip into the viewer.
///
/// Thread-safety: GUI thread only.
class MediaHoverPreview : public QObject
{
    Q_OBJECT
public:
    /// Time after which a request in flight is given up (a failed decode sends no signal).
    static constexpr int kGiveUpMs = 1500;

    /// Shows its previews in @p viewer (a Source mode viewer; not owned).
    explicit MediaHoverPreview(ViewerPanel* viewer, QObject* parent = nullptr);

    /// The pointer is at @p position (0..1, clamped) along the thumbnail of the clip file
    /// @p path of @p durationUs (0 = a still image).
    void hover(const QString& path, int64_t durationUs, double position);
    /// Ends the preview: the viewer shows its own picture again.
    void stop();

    /// Clip file being previewed (empty = none).
    [[nodiscard]] QString hoveredPath() const { return m_path; }
    /// Time (µs) wanted now.
    [[nodiscard]] int64_t wantedTimeUs() const { return m_timeUs; }
    /// True while the viewer shows a hover picture.
    [[nodiscard]] bool showing() const { return m_showing; }
    /// Times (µs) asked of the media cache since construction (tests).
    [[nodiscard]] const QVector<int64_t>& requestedTimes() const { return m_requested; }

private:
    /// Shows the wanted frame if cached, else asks for it unless a request is in flight.
    void update();
    void onFrameReady(const QString& path);
    /// Follows the viewer's engine: playback ends the preview.
    void watchEngine();
    [[nodiscard]] bool viewerBusy() const;

    QPointer<ViewerPanel> m_viewer;
    QPointer<PlaybackEngine> m_engine;
    QString m_path;
    int64_t m_timeUs = 0;
    bool m_showing = false;
    bool m_inFlight = false;
    QString m_flightPath;
    QTimer m_giveUp;
    QVector<int64_t> m_requested;
};

} // namespace kk
