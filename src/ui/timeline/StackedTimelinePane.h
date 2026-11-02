// SPDX-License-Identifier: GPL-3.0-or-later
// StackedTimelinePane — the second, inactive timeline of the Stacked Timelines view (View >
// Stacked Timelines): its own tabs, playhead, zoom and scroll, drawn read-only with the
// timeline painter. A click activates it (the panel swaps the editor in); dragging a clip
// out of it copies the clip into the active timeline; clips dragged in are copied into it.
#pragma once

#include "core/TimelineData.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePainter.h"

#include <QAbstractScrollArea>
#include <QSet>
#include <QWidget>

#include <memory>

class QMimeData;
class QTabBar;
class QToolButton;

namespace kk {

struct AppContext;
class NestedPreview;

/// Read-only view of one inactive timeline (the canvas of StackedTimelinePane).
/// Thread-safety: GUI thread only.
class StackedTimelineView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit StackedTimelineView(AppContext& ctx, QWidget* parent = nullptr);

    /// Shows timeline @p uid (0 = nothing) with its remembered zoom / scroll.
    void setTimeline(quint64 uid);
    [[nodiscard]] quint64 timelineUid() const { return m_uid; }
    /// Current geometry (tests).
    [[nodiscard]] const TimelineLayout& layout() const { return m_layout; }
    /// Stores the zoom / scroll into the project (Project::setViewState()).
    void storeViewState();
    /// Re-reads the timeline (after project changes).
    void refresh();

signals:
    /// A click on the view: the panel makes this timeline the active one. @p pos in viewport
    /// coordinates.
    void activateRequested();
    /// Clips of the active timeline were dropped here (MIME mime::TimelineClips): the panel
    /// activates this timeline and copies them to @p frame on @p track (insert with Ctrl).
    void clipsDropped(quint64 sourceUid, const QVector<quint64>& clips, int64_t frame,
        kk::TrackRef track, bool insert);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void updateScrollBars();
    void zoomToFit();
    /// Ghost of a TimelineClips drop at @p pos (empty when the payload does not apply).
    bool planDrop(const QMimeData* mime, QPointF pos, int64_t* frame, TrackRef* track,
        quint64* sourceUid, QVector<quint64>* clips);

    AppContext& m_ctx;
    quint64 m_uid = 0;
    std::shared_ptr<const TimelineData> m_data;
    TimelineLayout m_layout;
    TimelinePainter m_painter;
    TimelineOverlay m_overlay;
    QSet<quint64> m_selected; ///< always empty (the inactive timeline has no selection)
    NestedPreview* m_nestedPreview = nullptr;
    bool m_fitPending = true;
    bool m_syncingScrollBars = false;
    // Press on a clip: drag it out (copy) once it moves past the drag distance.
    QPointF m_pressPos;
    quint64 m_pressClip = 0;
    bool m_pressed = false;
};

/// The pane: tab row (own list of timelines, a "+" button to open another one) above the
/// StackedTimelineView. Thread-safety: GUI thread only.
class StackedTimelinePane : public QWidget
{
    Q_OBJECT
public:
    explicit StackedTimelinePane(AppContext& ctx, QWidget* parent = nullptr);

    /// The pane's tabs (timeline uids) and the one shown.
    void setTabs(const QVector<quint64>& tabs, quint64 current);
    [[nodiscard]] const QVector<quint64>& tabs() const { return m_tabs; }
    [[nodiscard]] quint64 currentTimeline() const { return m_view->timelineUid(); }
    /// Shows timeline @p uid (added to the tabs when missing).
    void showTimeline(quint64 uid);
    [[nodiscard]] StackedTimelineView* view() const { return m_view; }
    [[nodiscard]] QTabBar* tabBar() const { return m_bar; }

signals:
    void activateRequested();
    void clipsDropped(quint64 sourceUid, const QVector<quint64>& clips, int64_t frame,
        kk::TrackRef track, bool insert);

private:
    void syncTabs();
    void showOpenMenu();

    AppContext& m_ctx;
    QVector<quint64> m_tabs;
    QTabBar* m_bar = nullptr;
    QToolButton* m_open = nullptr;
    StackedTimelineView* m_view = nullptr;
    bool m_syncing = false;
};

} // namespace kk
