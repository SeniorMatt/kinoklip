// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineLayout — pure geometry of the timeline view: frame <-> x, track rows, hit-testing.
// No widgets, no painting: everything here is unit-tested (tests/timeline/test_timelinelayout).
#pragma once

#include "core/TimelineData.h"

#include <QHash>
#include <QPointF>
#include <QRect>
#include <QSize>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace kk {

/// Fixed metrics of the timeline view (logical pixels).
namespace timelinemetrics {
inline constexpr int kRulerHeight = 30; ///< time ruler at the top of the view
inline constexpr int kHeaderWidth = 196; ///< track header column on the left
inline constexpr int kDefaultVideoHeight = 66; ///< TrackData::height == 0 for video
inline constexpr int kDefaultAudioHeight = 50; ///< TrackData::height == 0 for audio
inline constexpr int kDefaultSubtitleHeight = 40; ///< TrackData::height == 0 for subtitles
inline constexpr int kMinTrackHeight = 26;
inline constexpr int kMaxTrackHeight = 240;
inline constexpr int kSectionGap = 6; ///< divider between the video and the audio tracks
inline constexpr int kEdgeGrabPx = 7; ///< width of the trim zones at clip edges
/// Half-width of the roll zone centred on a cut between two touching clips (spec 8.2/8.5).
inline constexpr int kRollZonePx = 2;
/// Height of the clip name band at the top of a clip (trim tool: slide zone, spec 8.6).
inline constexpr int kClipNameBandPx = 15;
inline constexpr int kResizeGrabPx = 3; ///< header bottom edge that resizes a track
inline constexpr int kHeaderButton = 18; ///< square header buttons (lock, mute, ...)
inline constexpr int kHeaderButtonGap = 3;
/// Audio page headers: width of the level meter at the right edge of audio track headers.
inline constexpr int kHeaderMeterWidth = 12;
/// Audio page headers at least this tall put their buttons on a second line (spec 37.3).
inline constexpr int kTwoLineHeaderMinHeight = 2 * kHeaderButton + 14;
/// Vertical gap between the two header lines.
inline constexpr int kHeaderLineGap = 4;
/// Zoom limits in pixels per frame: ~2.5 h of 25 fps per 1000 px .. 48 px per frame.
inline constexpr double kMinPixelsPerFrame = 0.004;
inline constexpr double kMaxPixelsPerFrame = 48.0;
} // namespace timelinemetrics

/// One track row of the view.
struct TimelineRow
{
    TrackRef ref;
    int top = 0; ///< content y (0 = first row below the ruler, before vertical scrolling)
    int height = 0; ///< whole row, keyframe / curve lane included
    /// Height of the keyframe / curve editor lane at the bottom of the row (0 = none); the
    /// clips use the remaining height - laneHeight at the top.
    int laneHeight = 0;
};

/// Buttons painted in a track header, from right to left.
enum class HeaderButton {
    None,
    Target, ///< destination patch for insert/overwrite (the "V1"/"A1" label)
    Lock,
    Visible, ///< video: track enabled in the output
    Mute, ///< audio
    Solo, ///< audio
    AutoSelect, ///< auto track selector (TrackData::autoSelect, spec 12.5)
    Arm ///< audio, Audio page headers only: record arm (disabled; recording is deferred)
};

/// Which part of a clip a point is on.
enum class ClipPart {
    None,
    Body,
    Head, ///< left trim zone
    Tail ///< right trim zone
};

/// Result of TimelineLayout::hitTest().
struct TimelineHit
{
    enum class Area {
        None, ///< outside everything (e.g. the top-left corner)
        Ruler,
        Header,
        HeaderResize, ///< bottom edge of a header: resize the track
        Track, ///< empty part of a track row
        Clip
    };
    Area area = Area::None;
    int row = -1; ///< index into TimelineLayout::rows(), -1 if none
    TrackRef track;
    quint64 clipId = 0; ///< the clip hit (Area::Clip)
    ClipPart part = ClipPart::None;
    /// Edit point between two touching clips: both ids set, clipId is the side the pointer
    /// is on (Tail of cutLeft or Head of cutRight).
    quint64 cutLeft = 0;
    quint64 cutRight = 0;
    HeaderButton button = HeaderButton::None;
    double frame = 0; ///< timeline frame under the pointer (fractional)
};

/// Geometry of the timeline view: horizontal zoom/scroll, track rows and hit-testing.
///
/// Coordinates: "view" coordinates are the widget's (ruler at y = 0, headers at x = 0).
/// Horizontal: x = kHeaderWidth + frame * pixelsPerFrame - scrollX.
/// Vertical: rows are stacked V(n)..V1, a divider, A1..A(n) (V1 and A1 meet in the middle,
/// like a sync line); row view y = kRulerHeight + verticalOffset() + row.top - scrollY.
/// When all rows fit, verticalOffset() places the V/A divider near the middle of the view.
///
/// Thread-safety: plain value type, no shared state.
class TimelineLayout
{
public:
    /// Rebuilds the row list from @p data (call after every timeline change).
    void rebuild(const TimelineData& data);
    /// Extra lane heights below tracks (TrackData::id -> px) for the keyframe and curve
    /// editors (KeyframeLanes); applied by the next rebuild().
    void setLaneHeights(QHash<quint64, int> heights) { m_laneHeights = std::move(heights); }
    /// Sets the widget size (view coordinates).
    void setViewSize(QSize size) { m_viewSize = size; }
    [[nodiscard]] QSize viewSize() const { return m_viewSize; }
    /// View-only vertical zoom applied to every track height (Shift+wheel, spec 19.7); not
    /// saved and not an undo step. Clamped to [kMinHeightScale, kMaxHeightScale]; takes
    /// effect on the next rebuild().
    void setHeightScale(double scale);
    [[nodiscard]] double heightScale() const { return m_heightScale; }
    /// Extra height factor of the audio rows only (compact audio, Cut page); takes effect on
    /// the next rebuild().
    void setAudioHeightScale(double scale) { m_audioHeightScale = std::clamp(scale, 0.1, 3.0); }
    [[nodiscard]] double audioHeightScale() const { return m_audioHeightScale; }
    /// Extra height factor of the video and subtitle rows (below 1 = collapsed, the Audio
    /// page; never below kMinTrackHeight); takes effect on the next rebuild().
    void setVideoHeightScale(double scale) { m_videoHeightScale = std::clamp(scale, 0.1, 1.0); }
    /// Rows start at the top of the view even when they all fit (no centred divider; the
    /// Audio page).
    void setRowsFromTop(bool on) { m_rowsFromTop = on; }
    [[nodiscard]] bool rowsFromTop() const { return m_rowsFromTop; }
    [[nodiscard]] double videoHeightScale() const { return m_videoHeightScale; }
    /// Audio page headers: audio tracks get a record arm button and a level meter at the
    /// right edge of their header (buttons move left to make room).
    void setAudioPageHeaders(bool on) { m_audioPageHeaders = on; }
    [[nodiscard]] bool audioPageHeaders() const { return m_audioPageHeaders; }
    static constexpr double kMinHeightScale = 0.5;
    static constexpr double kMaxHeightScale = 3.0;

    // ---- Horizontal ----

    [[nodiscard]] double pixelsPerFrame() const { return m_pixelsPerFrame; }
    /// Sets the zoom (clamped to the metric limits) keeping the frame under view x
    /// @p anchorX at the same position. Returns the new scrollX (not clamped to content).
    double setPixelsPerFrame(double pixelsPerFrame, double anchorX);
    /// Horizontal scroll in pixels (0 = frame 0 at the left edge of the track area).
    [[nodiscard]] double scrollX() const { return m_scrollX; }
    void setScrollX(double px) { m_scrollX = px < 0 ? 0 : px; }
    /// View x of timeline frame @p frame (fractional frames allowed).
    [[nodiscard]] double frameToX(double frame) const
    {
        return timelinemetrics::kHeaderWidth + frame * m_pixelsPerFrame - m_scrollX;
    }
    /// Fractional timeline frame at view x @p x.
    [[nodiscard]] double xToFrame(double x) const
    {
        return (x - timelinemetrics::kHeaderWidth + m_scrollX) / m_pixelsPerFrame;
    }
    /// Frame boundary nearest to view x @p x (edit points snap to frame boundaries), >= 0.
    [[nodiscard]] int64_t nearestFrameAtX(double x) const;
    /// Frame containing view x @p x (floor), >= 0 (for the playhead and blade).
    [[nodiscard]] int64_t frameAtX(double x) const;
    /// Width of the track area in pixels.
    [[nodiscard]] int trackAreaWidth() const
    {
        return std::max(0, m_viewSize.width() - timelinemetrics::kHeaderWidth);
    }
    /// First (floor) and last (ceil) frames visible in the track area.
    [[nodiscard]] std::pair<int64_t, int64_t> visibleFrames() const;
    /// Zoom that shows @p frames frames across the track area (with a small margin).
    [[nodiscard]] double fitPixelsPerFrame(int64_t frames) const;

    // ---- Vertical ----

    [[nodiscard]] const std::vector<TimelineRow>& rows() const { return m_rows; }
    /// Total height of all rows plus the divider.
    [[nodiscard]] int contentHeight() const { return m_contentHeight; }
    /// Height available for rows (view height minus ruler).
    [[nodiscard]] int rowAreaHeight() const
    {
        return std::max(0, m_viewSize.height() - timelinemetrics::kRulerHeight);
    }
    /// Largest useful vertical scroll.
    [[nodiscard]] int maxScrollY() const { return std::max(0, m_contentHeight - rowAreaHeight()); }
    [[nodiscard]] int scrollY() const { return m_scrollY; }
    void setScrollY(int px) { m_scrollY = std::clamp(px, 0, maxScrollY()); }
    /// Extra offset applied when the rows fit (centres the V/A divider), else 0 (always 0 with
    /// rowsFromTop()).
    [[nodiscard]] int verticalOffset() const;
    /// View y of the divider between video and audio rows.
    [[nodiscard]] int dividerY() const;
    /// View rect of row @p index across the whole width (headers included).
    [[nodiscard]] QRect rowRect(int index) const;
    /// Row index at view y @p y, or -1.
    [[nodiscard]] int rowAtY(double y) const;
    /// Row index of track @p ref, or -1.
    [[nodiscard]] int rowOf(TrackRef ref) const;
    /// Effective height of a track (its TrackData::height or the default for its type).
    [[nodiscard]] static int trackHeight(const TrackData& track);

    // ---- Headers ----

    /// Buttons shown in the header of a track of @p type, ordered from right to left.
    [[nodiscard]] static const std::vector<HeaderButton>& headerButtons(TrackType type);
    /// The buttons this layout shows for @p type (headerButtons(), plus Arm for audio tracks
    /// with audio page headers).
    [[nodiscard]] const std::vector<HeaderButton>& buttonsFor(TrackType type) const;
    /// View rect of the header level meter of row @p row (empty unless audio page headers
    /// and an audio row).
    [[nodiscard]] QRect headerMeterRect(int row) const;
    /// True if row @p row's header shows its buttons on a second line, under the name
    /// (Audio page headers of tall audio rows, so the track name stays readable).
    [[nodiscard]] bool twoLineHeader(int row) const;
    /// View rect of @p button in the header of row @p row (empty if the type has no such
    /// button).
    [[nodiscard]] QRect headerButtonRect(int row, HeaderButton button) const;
    /// View rect of the track name in the header of row @p row, between the V1 / A1 patch and
    /// the buttons (double-click renames the track there, spec 12.15; empty if no room).
    [[nodiscard]] QRect headerNameRect(int row) const;

    // ---- Clips ----

    /// Indices [first, last) of the clips of @p track overlapping frames [f0, f1).
    /// Two binary searches: clips are sorted and disjoint, so their ends are sorted too.
    [[nodiscard]] static std::pair<size_t, size_t> clipRange(
        const TrackData& track, double f0, double f1);
    /// View rect of @p clip in row @p row (not clipped to the visible area; 1 px inset
    /// vertically so neighbouring rows do not touch; above the row's lane).
    [[nodiscard]] QRectF clipRect(int row, const ClipData& clip) const;
    /// View rect of the keyframe / curve lane of row @p index (empty if it has none).
    [[nodiscard]] QRect laneRect(int index) const;

    /// What is at view position @p pos.
    [[nodiscard]] TimelineHit hitTest(const TimelineData& data, QPointF pos) const;

private:
    QSize m_viewSize;
    double m_pixelsPerFrame = 2.0;
    double m_heightScale = 1.0;
    double m_audioHeightScale = 1.0; ///< setAudioHeightScale()
    double m_videoHeightScale = 1.0; ///< setVideoHeightScale()
    bool m_audioPageHeaders = false; ///< setAudioPageHeaders()
    bool m_rowsFromTop = false; ///< setRowsFromTop()
    double m_scrollX = 0;
    int m_scrollY = 0;
    std::vector<TimelineRow> m_rows;
    QHash<quint64, int> m_laneHeights;
    int m_videoHeight = 0; ///< height of all video rows (above the divider)
    int m_contentHeight = 0;
};

} // namespace kk
