// SPDX-License-Identifier: GPL-3.0-or-later
// GuideOverlay — viewer guides, safe areas, aspect marker and grid drawn over the picture of a
// viewer (never rendered), and the mouse gestures that add, move and remove guides.
#pragma once

#include "core/ViewerGuides.h"

#include <QPointF>
#include <QPointer>
#include <QTransform>
#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

namespace kk {

struct AppContext;
class ViewerSurface;

/// Display-only layer over a viewer's picture surface (spec 41).
///
/// - **What it draws**: the guides (Show Guides, colour from ViewerOverlaySettings), the
///   action / title safe areas, the centre mark, the aspect-ratio marker and the grid, all in
///   timeline frame px mapped through the surface's pictureRect() (Fit / zoom), so they stay
///   on the same picture point at any viewer size or zoom.
/// - **Never in a render**: it is a child widget over the surface. The renderers, the export,
///   --render-frame and the compositor's frames never see it; grabs of the viewer picture
///   (the source drag pixmap) and the --screenshot window grab hide it first (setSuppressed()).
/// - **Where the guides live**: Timeline viewers edit TimelineData::guides of the active
///   timeline (saved with the project; each change is one undo step: "Add Guide", "Move
///   Guide", "Remove Guide", "Clear Guides", "Load Guide Preset", "Set Guide Position").
///   Source viewers keep one guide list per user (QSettings "viewer/sourceGuides", in px of
///   the clip shown), because a media clip has no document to store them in.
/// - **Mouse**: the overlay itself is transparent for the mouse so the other overlays and the
///   surface keep working; it filters the events of the surface and its child overlays
///   instead. Near an unlocked guide (kGrabPx) it changes the cursor, a press drags the guide,
///   a double-click opens the position dialog, the context menu offers Edit / Remove. A
///   guide released outside the picture or outside the surface (onto a ruler) is removed.
///   Esc during a drag cancels it. ViewerRuler starts new guides with beginNewGuide().
///
/// Thread-safety: GUI thread only.
class GuideOverlay : public QWidget
{
    Q_OBJECT
public:
    /// Distance (widget px) within which a guide is grabbed.
    static constexpr double kGrabPx = 12.0;
    /// Within this reach a guide wins over the controls below it; between this and kGrabPx a
    /// control handle under the pointer goes first.
    static constexpr double kExactGrabPx = 4.0;

    /// Creates the layer over @p surface (its widget becomes the parent). @p timeline: guides
    /// of the project's active timeline; false: the source viewer's own list.
    GuideOverlay(AppContext& ctx, ViewerSurface* surface, bool timeline);
    ~GuideOverlay() override;

    /// True for a timeline viewer's overlay.
    [[nodiscard]] bool isTimeline() const { return m_timeline; }
    /// The guides currently shown (during a drag: with the dragged guide where it is now).
    [[nodiscard]] std::vector<ViewerGuide> guides() const;
    /// The stored guides (without a drag in progress).
    [[nodiscard]] std::vector<ViewerGuide> storedGuides() const;
    /// Replaces the stored guides; timeline viewers make it one undo step named @p undoLabel.
    void setGuides(const std::vector<ViewerGuide>& list, const QString& undoLabel);
    /// Native picture size in frame px: the timeline resolution, or (source viewers) the size
    /// given to setFrameSize(); empty while a source viewer shows no clip (nothing is drawn).
    [[nodiscard]] QSize frameSize() const;
    /// Source viewers: native size of the clip shown (rulers and guide units); empty = none.
    void setFrameSize(QSize size);
    /// Frame px -> widget px of this overlay (= of the surface widget).
    [[nodiscard]] QTransform frameToWidget() const;
    /// Widget px of the overlay -> frame px.
    [[nodiscard]] QTransform widgetToFrame() const { return frameToWidget().inverted(); }

    /// Adds a guide through the centre of the picture (View > Guides > Add ...) and turns
    /// Show Guides on. @p position: frame px instead of the centre.
    void addGuide(Qt::Orientation orientation, std::optional<double> position = std::nullopt);
    /// Removes guide @p index.
    void removeGuide(int index);
    /// Removes every guide (View > Guides > Clear Guides).
    void clearGuides();
    /// Replaces the guides by @p preset (fractions of this viewer's frame).
    void applyPreset(const GuidePreset& preset);
    /// Opens the position dialog of guide @p index (px or % of the frame).
    void editGuidePosition(int index);

    /// Index of the guide under widget point @p pos (within kGrabPx), or -1. Hidden guides
    /// are never hit; locked ones are (for the context menu) unless @p editableOnly.
    [[nodiscard]] int guideAt(
        QPointF pos, bool editableOnly = true, double reachPx = kGrabPx) const;
    /// Tells the overlay whether a control handle sits under a widget point of the surface.
    void setControlProbe(std::function<bool(QPointF)> probe) { m_controlAt = std::move(probe); }
    /// True while guides can be changed with the mouse (shown and not locked).
    [[nodiscard]] bool mouseEditable() const;

    // ---- Drags (also driven by ViewerRuler; positions in this overlay's widget px) ----

    /// Starts dragging a new guide out of a ruler.
    void beginNewGuide(Qt::Orientation orientation, QPointF pos);
    /// Starts dragging existing guide @p index.
    void beginMove(int index, QPointF pos);
    /// Moves the dragged guide to @p pos; with Ctrl in @p modifiers it snaps onto the frame
    /// edges, centre, thirds, quarters, safe areas, grid, aspect marker and other guides.
    void dragTo(QPointF pos, Qt::KeyboardModifiers modifiers = {});
    /// Ends the drag at @p pos: stores the guide, or removes it (or drops a new one) if
    /// @p pos is outside the surface or the guide outside the frame. @p commit false cancels.
    void finishDrag(QPointF pos, bool commit = true);
    /// True while a guide is being dragged.
    [[nodiscard]] bool isDragging() const { return m_drag.has_value(); }
    /// The guide being dragged, where it is now (frame px); nullptr when not dragging.
    [[nodiscard]] const ViewerGuide* draggedGuide() const
    {
        return m_drag ? &m_drag->guide : nullptr;
    }

    /// Positions (frame px) the on-screen transform controls snap to on @p orientation's
    /// axis: the shown guides when Show Guides and Snap to Guides are on, else none.
    [[nodiscard]] std::vector<double> snapTargets(Qt::Orientation orientation) const;

    /// Hides every guide overlay of the process (picture grabs, the --screenshot window
    /// grab); restore with false.
    static void setSuppressed(bool suppressed);
    [[nodiscard]] static bool isSuppressed();

    /// Re-evaluates geometry and repaints (zoom, timeline or option changes).
    void refresh();

Q_SIGNALS:
    /// The user pressed on a guide (the viewer activates itself).
    void pressed();
    /// The guides, the drag, the frame size or the picture mapping changed (rulers repaint).
    void changed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// The guide a press or hover at @p pos takes: see kExactGrabPx.
    [[nodiscard]] int guideToTake(QPointF pos) const;

    std::function<bool(QPointF)> m_controlAt;

    struct Drag
    {
        int index = -1; ///< guide being moved; -1 = a new guide from a ruler
        ViewerGuide guide; ///< where it is now (frame px)
        bool remove = false; ///< released here it would be removed
        QPointF pointer; ///< last pointer position (overlay px), to re-snap when Ctrl changes
        bool snap = false; ///< Ctrl held: the guide snaps (guides::guideSnapTargets)
    };
    /// Handles a mouse / context event of @p widget (the surface or a child overlay).
    bool filterMouse(QWidget* widget, QEvent* event);
    void watch(QObject* child);
    /// @p pos of @p widget (the surface or one of its children) in surface coordinates.
    [[nodiscard]] QPointF toHost(QWidget* widget, QPointF pos) const;
    void updateDrag(QPointF pos);
    void storeSourceGuides(const std::vector<ViewerGuide>& list);
    void paintSafeAreas(QPainter& p, const QTransform& map, QSizeF frame) const;
    void paintGrid(QPainter& p, const QTransform& map, QSizeF frame) const;
    void paintGuides(QPainter& p, const QTransform& map, QSizeF frame) const;

    AppContext& m_ctx;
    ViewerSurface* m_surface;
    bool m_timeline;
    QSize m_frameSize; ///< source viewers: setFrameSize()
    std::vector<ViewerGuide> m_sourceGuides; ///< source viewers' list
    std::optional<Drag> m_drag;
    QPointer<QWidget> m_cursorWidget; ///< widget whose cursor shows a guide hover
};

} // namespace kk
