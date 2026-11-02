// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget — the interactive, custom-painted timeline: ruler, track headers, clips,
// tools (select / trim / blade), zoom and scroll, drag and drop.
#pragma once

#include "core/EditOps.h"
#include "core/TimelineData.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelineMedia.h"
#include "ui/timeline/TimelinePainter.h"

#include <QAbstractScrollArea>
#include <QSet>
#include <QTimer>

#include <cstdint>
#include <optional>
#include <vector>

class QMimeData;

namespace kk {

struct AppContext;
class KeyframeLanes;
class TimelineController;

/// The timeline editor view.
///
/// Drawing is delegated to TimelinePainter, geometry to TimelineLayout; this class owns the
/// input state machine (see src/ui/timeline/readme.md) and turns gestures into EditOps.
/// Every gesture is one undo step: the edit opens when a drag starts, the timeline is
/// restored to the press-time state and the operation re-applied on every mouse move (so
/// overwrite semantics stay exact while dragging), and the edit closes on release (Esc
/// cancels it).
///
/// Implementation is split over TimelineWidget.cpp (setup, paint, zoom, scroll, menus),
/// TimelineInput.cpp (mouse state machine) and TimelineDrop.cpp (drag and drop).
/// Thread-safety: GUI thread only.
class TimelineWidget : public QAbstractScrollArea
{
    Q_OBJECT
public:
    /// Creates the view for @p ctx, driven by @p controller (not owned, must outlive it).
    TimelineWidget(AppContext& ctx, TimelineController* controller, QWidget* parent = nullptr);
    ~TimelineWidget() override;

    [[nodiscard]] TimelineController* controller() const { return m_controller; }
    /// Current geometry (for tests and hit-testing from outside).
    [[nodiscard]] const TimelineLayout& layout() const { return m_layout; }
    /// The per-clip keyframe and curve editors (KeyframeLanes.h).
    [[nodiscard]] KeyframeLanes* keyframeLanes() const { return m_keyframeLanes; }

    /// Replaces the thumbnail source (default: media::MediaCache). Null = placeholders.
    void setThumbnailProvider(ThumbnailProvider provider);
    /// Replaces the waveform source (default: media::MediaCache). Null = placeholders.
    void setWaveformProvider(WaveformProvider provider);

    /// Horizontal zoom in pixels per frame, keeping the frame under view x @p anchorX fixed.
    void setPixelsPerFrame(double pixelsPerFrame, double anchorX);
    [[nodiscard]] double pixelsPerFrame() const { return m_layout.pixelsPerFrame(); }
    /// Scrolls so that frame @p frame is at the left edge of the track area.
    void scrollToFrame(double frame);

    /// When on (default), a playhead that leaves the visible range (playback, keyboard
    /// navigation) scrolls the view by a page so it stays visible.
    void setFollowPlayhead(bool follow) { m_followPlayhead = follow; }
    [[nodiscard]] bool followPlayhead() const { return m_followPlayhead; }

public slots:
    void zoomIn();
    void zoomOut();
    /// Fits the whole timeline into the view (Shift+Z).
    void zoomToFit();
    /// Repaints clips after thumbnails/waveforms became available.
    void refreshMedia();
    /// Page-scrolls if the playhead is outside the visible range.
    void ensurePlayheadVisible();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    // ---- Input state machine (TimelineInput.cpp) ----
    enum class DragMode {
        None,
        Pending, ///< button down, waiting for the drag threshold; becomes `pending`
        Scrub,
        RubberBand,
        Move,
        TrimHead,
        TrimTail,
        Roll,
        Slip,
        Slide,
        ResizeTrack,
        Pan
    };
    struct Drag
    {
        DragMode mode = DragMode::None;
        DragMode pending = DragMode::None;
        QPointF pressPos;
        QPointF lastPos;
        TimelineHit hit;
        Qt::KeyboardModifiers modifiers;
        bool ripple = false; ///< trims: ripple (trim tool) or leave gaps (select tool)
        bool editOpen = false;
        bool selectOnRelease = false; ///< click on a selected clip without dragging
        TimelineData origin; ///< state at drag start; restored before every live update
        QVector<quint64> clips; ///< affected clips (primary first)
        QVector<std::pair<quint64, quint64>> rolls; ///< roll: (left, right) pairs
        quint64 primary = 0;
        TrackRef primaryTrack;
        int64_t primaryStart = 0;
        int64_t primaryEnd = 0;
        int64_t minStart = 0; ///< earliest start of the moved clips
        int lastDy = 0;
        editops::Clipboard board; ///< Alt-drag duplicate source
        bool duplicate = false;
        QVector<quint64> resultIds; ///< ids created by the gesture (duplicate)
        std::vector<int64_t> snaps; ///< snap candidates (dragged clips excluded)
        QVector<quint64> baseSelection; ///< rubber band with Shift/Ctrl keeps these
        int originHeight = 0;
        int scrollXAtPress = 0;
        int scrollYAtPress = 0;
    };

    void startDrag(DragMode mode);
    void updateDrag(QPointF pos);
    void finishDrag();
    void cancelDrag();
    void openEdit(const QString& label);
    void restoreOrigin();
    void updateMove(QPointF pos);
    void updateTrim(QPointF pos);
    void updateRoll(QPointF pos);
    void updateSlipSlide(QPointF pos);
    void updateRubberBand(QPointF pos);
    void scrubTo(QPointF pos);
    void bladeAt(const TimelineHit& hit);
    void clickSelect(const TimelineHit& hit, Qt::KeyboardModifiers modifiers);
    void headerClick(const TimelineHit& hit);
    void updateHover(QPointF pos);
    void autoScrollTick();
    /// Snaps @p frame to the drag's candidates when snapping is on (and sets the indicator).
    [[nodiscard]] std::optional<int64_t> snapFrame(int64_t frame) const;
    [[nodiscard]] int64_t snapToleranceFrames() const;

    // ---- Drag and drop (TimelineDrop.cpp) ----
    struct DropPlan
    {
        enum class Kind {
            None,
            Clips,
            Effect,
            Transition
        };
        Kind kind = Kind::None;
        /// Clips: groups of clips created from one source each (linked after placing).
        QVector<QVector<editops::PlacedClip>> groups;
        bool insert = false; ///< Ctrl: insert instead of overwrite
        quint64 clipId = 0; ///< Effect/Transition target
        QString effectId;
        bool head = true; ///< Transition: at the clip head (transitionIn) or tail
        int64_t transitionFrames = 0;
        int64_t previewFrom = 0;
        int64_t previewTo = 0;
        int previewRow = -1;
    };
    [[nodiscard]] DropPlan planDrop(
        const QMimeData* mime, QPointF pos, Qt::KeyboardModifiers modifiers);
    void showDropPreview(const DropPlan& plan);
    bool executeDrop(const DropPlan& plan);
    void clearDropPreview();

    // ---- Helpers (TimelineWidget.cpp) ----
    void onTimelineChanged();
    void onSelectionChanged();
    void onPlayheadChanged(int64_t frame);
    void updateScrollBars();
    void zoomBy(double factor, double anchorX);
    [[nodiscard]] double zoomAnchorX() const;

    AppContext& m_ctx;
    TimelineController* m_controller = nullptr;
    TimelineLayout m_layout;
    TimelinePainter m_painter;
    KeyframeLanes* m_keyframeLanes = nullptr; ///< child QObject: keyframe / curve editor lanes
    TimelineOverlay m_overlay;
    QSet<quint64> m_selected; ///< mirror of the SelectionModel for O(1) lookups while painting
    Drag m_drag;
    QTimer m_autoScroll;
    bool m_followPlayhead = true;
    bool m_syncingScrollBars = false;
    bool m_fitOnFirstShow = true;
};

} // namespace kk
