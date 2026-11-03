// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePainter — draws the whole timeline view (ruler, headers, clips, overlays) for one
// paint event. Separated from the widget so that drawing has no input/state-machine logic.
#pragma once

#include "core/SelectionModel.h"
#include "core/TimelineData.h"
#include "ui/timeline/DragInfo.h"
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
#include <functional>
#include <optional>
#include <vector>

class QPainter;
class QPalette;

namespace kk {

struct MediaItem;
class TimelineAnimator;

class MediaPool;

/// A clip that would be created by a drop or drag (drawn translucent).
struct TimelineGhost
{
    int row = -1; ///< -1: a track the drop will create, drawn in @c band
    QRect band;
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
    DragInfoLabel dragInfo; ///< offset label next to the pointer while moving / trimming
};

/// Everything a paint needs, gathered by the widget (all pointers non-owning, non-null except
/// mediaPool).
/// One range of the render cache status bar on the ruler (Resolve: red = needs caching,
/// blue = cached).
struct CacheBarRange
{
    int64_t start = 0;
    int64_t end = 0; ///< exclusive
    bool cached = false;
};

/// What video clips show below their name band (Timeline View Options, spec 12.11).
enum class ClipThumbnails {
    Filmstrip, ///< a thumbnail every tile width across the clip (default)
    HeadTail, ///< the first frame at the clip start and the last frame at its end
    None, ///< no pictures: name band and clip colour only (fastest)
};

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
    /// Selected edit point (spec 7.6): the clip whose tail and/or the clip whose head is
    /// selected (both = a roll); 0 = that side not selected.
    quint64 editTailClip = 0;
    quint64 editHeadClip = 0;
    /// Selected gap (spec 10.5): row and frame range; row -1 = none.
    int gapRow = -1;
    int64_t gapStart = 0;
    int64_t gapEnd = 0;
    /// Selected transition (spec 13.10); clipId 0 = none.
    TransitionRef transition;
    /// Render cache status (Playback > Render Cache); null / empty = no bar.
    const QVector<CacheBarRange>* cache = nullptr;
    /// Clips showing their retime controls (Ctrl+R); nullptr = none.
    const QSet<quint64>* retimeShown = nullptr;
    /// Clip under the mouse (its fade handles are shown); 0 = none.
    quint64 hoverClip = 0;
    /// Selected keyframes (volume keys on the level lines are drawn selected); may be null.
    const QVector<KeyframeRef>* selectedKeys = nullptr;
    /// Timelines of the project by uid (multicam clips show their active angle's name);
    /// nullptr = plain clip names.
    const std::function<const TimelineData*(quint64)>* timelines = nullptr;
    /// Audio page header meters: dBFS (left, right) of an audio track by id; nullptr = none.
    const std::function<std::pair<float, float>(quint64)>* headerMeter = nullptr;
    /// Running display animations (TimelineAnimator.h): clips are drawn at their tweened
    /// rects, plus ghosts, flashes, the playhead glide and the snap fade. nullptr (animations
    /// off or idle) = everything is drawn exactly at the model.
    const TimelineAnimator* anim = nullptr;
    /// Video clip pictures (Timeline View Options).
    ClipThumbnails thumbnails = ClipThumbnails::Filmstrip;
    /// Audio clips draw their waveform (Timeline View Options); off = clip colour only.
    bool waveforms = true;
    /// Media and nested clips show their source in timecode after the name (Timeline View
    /// Options > Show Source Timecode, spec 12.14); off by default like Resolve.
    bool sourceTimecode = false;
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

    /// Where the "fx" badge of a clip with effects is drawn: @p clipRect from
    /// TimelineLayout::clipRect(), @p viewRight the right edge of the view (the badge sticks to
    /// the visible right end of the clip). Empty if the clip is too narrow to show it.
    [[nodiscard]] static QRectF fxBadgeRect(const QRectF& clipRect, double viewRight);
    /// Where the node badge of a clip with a composition (Compositing page) is drawn: left of
    /// the fx badge when the clip also has effects (@p hasFx), else in its place. Empty if the
    /// clip is too narrow.
    [[nodiscard]] static QRectF compBadgeRect(const QRectF& clipRect, double viewRight, bool hasFx);

    /// Updates fonts and colours (call on palette/font change).
    void setStyle(const QPalette& palette, const QFont& font);

    /// Paints the whole view.
    void paint(QPainter& p, const TimelinePaintInput& in);

    /// Body colour of a clip (label colour, or a colour by clip kind and track type).
    [[nodiscard]] static QColor clipColor(const ClipData& clip, TrackType type);
    /// Body colour as drawn: the clip's own label, else the clip colour of its media pool
    /// item @p item (may be null), else the colour by kind and track type.
    [[nodiscard]] static QColor clipColor(
        const ClipData& clip, TrackType type, const MediaItem* item);

private:
    /// Media path (or nested-preview pseudo path) of @p clip for the thumbnail / waveform
    /// providers.
    [[nodiscard]] static QString clipPreviewPath(const ClipData& clip, const MediaItem* item);
    void paintTrackArea(QPainter& p, const TimelinePaintInput& in);
    void paintRow(QPainter& p, const TimelinePaintInput& in, int row);
    void paintClip(QPainter& p, const TimelinePaintInput& in, int row, const TrackData& track,
        const ClipData& clip);
    /// paintClip() with the animation opacity of @p clip applied (in.anim set).
    void paintAnimatedClip(QPainter& p, const TimelinePaintInput& in, int row,
        const TrackData& track, const ClipData& clip);
    /// Ghosts of removed clips of track @p track (in.anim set).
    void paintGhosts(QPainter& p, const TimelinePaintInput& in, TrackRef track);
    /// Blade / trim flashes (in.anim set).
    void paintAnimationOverlays(QPainter& p, const TimelinePaintInput& in);
    /// @p rect (a model clip rect) as drawn: the animator's tweened rect when one runs.
    [[nodiscard]] static QRectF shownRect(
        const TimelinePaintInput& in, quint64 clipId, QRectF rect);
    void paintThumbnails(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
        const QRectF& content, const QRectF& visible);
    void paintWaveform(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
        const QRectF& content, const QRectF& visible, const QColor& color);
    /// The audio level line with its keys (LevelLines.h); @p clipRect = the whole clip.
    void paintVolumeLine(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
        const QRectF& clipRect, const QRectF& visible);
    /// The fade wedges and, on the hovered clip, the fade handles.
    void paintFades(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
        const QRectF& clipRect, const QRectF& visible);
    /// The speed bar, speed points and handles of a clip with retime controls (RetimeBar.h).
    void paintRetime(QPainter& p, const TimelinePaintInput& in, const ClipData& clip,
        const QRectF& rect, const QRectF& visible);
    void paintKeyframes(
        QPainter& p, const TimelinePaintInput& in, const ClipData& clip, const QRectF& rect);
    void paintTransitions(QPainter& p, const TimelinePaintInput& in, int row,
        const TrackData& track, size_t first, size_t last);
    void paintTransitionBlock(QPainter& p, const QRectF& r, bool in, bool selected = false);
    void paintHeaders(QPainter& p, const TimelinePaintInput& in);
    void paintRuler(QPainter& p, const TimelinePaintInput& in);
    void paintOverlays(QPainter& p, const TimelinePaintInput& in);
    void paintPlayhead(QPainter& p, const TimelinePaintInput& in);
    /// Name band text of @p clip: its name, or for a multicam segment the active angle
    /// (number, name) and the name (cached per clip).
    [[nodiscard]] const QString& clipLabel(const TimelinePaintInput& in, const ClipData& clip);
    /// @p text (the clip's label) elided to @p width px (cached per clip).
    /// Cached source in timecode of @p clip ("01:00:02:00"; empty for generated clips).
    [[nodiscard]] const QString& sourceTimecodeText(
        const TimelinePaintInput& in, const ClipData& clip);
    [[nodiscard]] const QString& elidedName(const ClipData& clip, const QString& text, int width);
    [[nodiscard]] QString mediaPath(const TimelinePaintInput& in, const QString& mediaId) const;

    ThumbnailProvider m_thumbnails;
    WaveformProvider m_waveforms;

    // Style
    QFont m_font;
    QFont m_smallFont;
    QFont m_boldFont;
    int m_fontAscent = 10;
    QColor m_base, m_window, m_text, m_textDim, m_accent, m_lane, m_laneAlt, m_line, m_timecode;
    QPixmap m_iconLock, m_iconUnlock, m_iconVisible, m_iconHidden, m_iconMute, m_iconSolo;

    // Caches / scratch buffers reused across paints.
    struct NameCache
    {
        QString name;
        int width = -1;
        QString elided;
        /// Multicam clips: the label "<n> <angle name> - <clip name>" and what it was made
        /// from (angle, multicam timeline data), rebuilt when either changes.
        int angle = -2;
        const TimelineData* multicam = nullptr;
        QString label;
        /// Source timecode text and the source time / timecode start it was made from.
        int64_t tcSourceUs = -1;
        int64_t tcStartUs = -1;
        QString tc;
    };
    QHash<quint64, NameCache> m_names;
    std::vector<float> m_peaks;
    std::vector<QLineF> m_lines;
};

} // namespace kk
