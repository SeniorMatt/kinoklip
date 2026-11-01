// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePainter — draws the whole timeline view (ruler, headers, clips, overlays) for one
// paint event. Separated from the widget so that drawing has no input/state-machine logic.
#pragma once

#include "core/TimelineData.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelineMedia.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QLineF>
#include <QPixmap>
#include <QRect>
#include <QSet>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

class QPainter;
class QPalette;

namespace kk {

class MediaPool;

/// A clip that would be created by a drop or drag (drawn translucent).
struct TimelineGhost
{
    int row = -1;
    int64_t start = 0;
    int64_t duration = 0;
    QString name;
};

/// Transient things drawn on top of the timeline (owned by the widget's input state machine).
struct TimelineOverlay
{
    std::optional<int64_t> snapFrame; ///< snap indicator line
    QRect rubberBand; ///< selection rectangle (view coordinates), empty = none
    std::optional<int64_t> bladeFrame; ///< blade tool hover line
    int bladeRow = -1;
    std::vector<TimelineGhost> ghosts; ///< drop preview
    quint64 dropClip = 0; ///< clip highlighted as an effect drop target
    int dropTransitionRow = -1; ///< transition drop preview: row and frame range
    int64_t dropTransitionFrom = 0;
    int64_t dropTransitionTo = 0;
};

/// Everything a paint needs, gathered by the widget (all pointers non-owning, non-null except
/// mediaPool).
struct TimelinePaintInput
{
    const TimelineData* data = nullptr;
    const TimelineLayout* layout = nullptr;
    const QSet<quint64>* selected = nullptr;
    const TimelineOverlay* overlay = nullptr;
    const MediaPool* mediaPool = nullptr;
    int64_t playhead = 0;
    int targetVideo = -1;
    int targetAudio = -1;
    bool hasFocus = false;
};

/// Draws the timeline. Keeps per-widget caches (elided clip names, icon pixmaps, waveform and
/// line buffers) so that steady-state painting does not allocate beyond QPainter itself.
///
/// Performance: only clips overlapping the visible frame range are visited (two binary
/// searches per track, TimelineLayout::clipRange()); sub-pixel clips are drawn as a single
/// fillRect; text, thumbnails and waveforms are only drawn when the clip is wide enough.
/// Thread-safety: GUI thread only.
class TimelinePainter
{
public:
    TimelinePainter();

    /// Sets the providers for thumbnails and waveforms (null = placeholders only).
    void setThumbnailProvider(ThumbnailProvider provider) { m_thumbnails = std::move(provider); }
    void setWaveformProvider(WaveformProvider provider) { m_waveforms = std::move(provider); }

    /// Updates fonts and colours (call on palette/font change).
    void setStyle(const QPalette& palette, const QFont& font);

    /// Paints the whole view.
    void paint(QPainter& p, const TimelinePaintInput& in);

    /// Body colour of a clip (label colour, or a colour by clip kind and track type).
    [[nodiscard]] static QColor clipColor(const ClipData& clip, TrackType type);

private:
    void paintTrackArea(QPainter& p, const TimelinePaintInput& in);
    void paintRow(QPainter& p, const TimelinePaintInput& in, int row);
    void paintClip(QPainter& p, const TimelinePaintInput& in, int row, const TrackData& track,
        const ClipData& clip);
    void paintThumbnails(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
        const QRectF& content, const QRectF& visible);
    void paintWaveform(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
        const QRectF& content, const QRectF& visible, const QColor& color);
    void paintVolumeLine(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
        const QRectF& full, const QRectF& visible);
    void paintKeyframes(
        QPainter& p, const TimelinePaintInput& in, const ClipData& clip, const QRectF& rect);
    void paintTransitions(QPainter& p, const TimelinePaintInput& in, int row,
        const TrackData& track, size_t first, size_t last);
    void paintTransitionBlock(QPainter& p, const QRectF& r, bool in);
    void paintHeaders(QPainter& p, const TimelinePaintInput& in);
    void paintRuler(QPainter& p, const TimelinePaintInput& in);
    void paintOverlays(QPainter& p, const TimelinePaintInput& in);
    void paintPlayhead(QPainter& p, const TimelinePaintInput& in);
    [[nodiscard]] const QString& elidedName(const ClipData& clip, int width);
    [[nodiscard]] QString mediaPath(const TimelinePaintInput& in, const QString& mediaId) const;

    ThumbnailProvider m_thumbnails;
    WaveformProvider m_waveforms;

    // Style
    QFont m_font;
    QFont m_smallFont;
    QFont m_boldFont;
    int m_fontAscent = 10;
    QColor m_base, m_window, m_text, m_textDim, m_accent, m_lane, m_laneAlt, m_line;
    QPixmap m_iconLock, m_iconUnlock, m_iconVisible, m_iconHidden, m_iconMute, m_iconSolo;

    // Caches / scratch buffers reused across paints.
    struct NameCache
    {
        QString name;
        int width = -1;
        QString elided;
    };
    QHash<quint64, NameCache> m_names;
    std::vector<float> m_peaks;
    std::vector<QLineF> m_lines;
};

} // namespace kk
