// SPDX-License-Identifier: GPL-3.0-or-later
// MaskOverlay — on-screen mask controls of the timeline viewer: outlines and handles of the
// current clip's masks drawn over the picture, and the mouse / key gestures that edit them.
#pragma once

#include "core/TimelineData.h"

#include <QPointF>
#include <QPointer>
#include <QSet>
#include <QTransform>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>

class QAction;
class QToolButton;

namespace kk {

struct AppContext;
class ViewerSurface;

/// Transparent child of a timeline viewer's picture surface (an additive layer: the surface
/// itself is not changed). It follows the viewer's Fit / zoom mapping through
/// ViewerSurface::pictureRect() and the clip's transform (render::clipTransform), so handles
/// sit on the picture at every zoom level and on transformed clips.
///
/// Shown when the on-screen mask controls are on (the "view.onscreen_masks" action, the
/// viewer's on-screen controls button at the bottom left) or a mask of the current clip is
/// active (SelectionModel::activeMask(), set by the Inspector's Masks section), while the
/// playhead is inside that clip on a video track. Otherwise it is hidden and transparent for
/// the mouse.
///
/// Gestures (each one undo step; docs/behavior-spec.md section 27):
/// * Rectangle / ellipse: drag inside = move (Shift: one axis), corner / edge handles = resize
///   with the opposite side fixed (Alt: from the centre, Shift: keep the aspect ratio), the
///   handle above the shape = rotate (Shift: 15 degree steps), the two triangles on the right
///   = feather outside / inside.
/// * Point masks: drag a point = move it (and the other selected points), click = select
///   (Shift adds), drag a tangent end = turn it (the other side follows on smooth points; Alt
///   breaks the tangent), double-click a point = corner / smooth, click on the outline = add a
///   point there, Delete / Backspace = remove the selected points, drag inside = move the mask.
///   Corner handles of the bounding box scale about the mask centre, the top handle rotates.
/// * Drawing (a new polygon / bezier mask): click = corner point, press and drag = smooth point
///   (bezier), click the first point or press Enter = close, Backspace = remove the last point,
///   Esc = close (or discard a mask with too few points). Freehand: press and draw; releasing
///   simplifies the stroke into a closed bezier path.
/// * Clicking the outline of another mask makes it the active one.
///
/// Thread-safety: GUI thread only.
class MaskOverlay : public QWidget
{
    Q_OBJECT
public:
    /// Creates the overlay over @p surface (its widget becomes the parent).
    MaskOverlay(AppContext& ctx, ViewerSurface* surface);
    ~MaskOverlay() override;

    /// True if the controls are shown and take the mouse (see the class comment).
    [[nodiscard]] bool isEditing() const { return m_editing; }
    /// The clip whose masks are shown (0 = none).
    [[nodiscard]] quint64 clipId() const { return m_clipId; }
    /// Clip content coordinates (timeline px, origin at the content centre) → overlay widget
    /// coordinates, or std::nullopt when nothing is shown. Tests use it to aim the mouse.
    [[nodiscard]] std::optional<QTransform> contentToWidget() const;
    /// Selected point indices of the active point mask.
    [[nodiscard]] const QSet<int>& selectedPoints() const { return m_selected; }

    /// The viewer's on-screen controls button (menu: Off / Transform / Crop / Mask, one mode
    /// at a time), driving @p masksAction, @p transformAction and @p cropAction
    /// (TransformOverlay; both may be null).
    [[nodiscard]] static QToolButton* createModeButton(QAction* masksAction, QWidget* parent,
        QAction* transformAction = nullptr, QAction* cropAction = nullptr);

    /// Re-evaluates visibility and repaints (zoom, aspect, timeline or selection changes).
    void refresh();

Q_SIGNALS:
    /// The user pressed on the overlay (the viewer activates itself).
    void pressed();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    /// What is under the mouse / being dragged.
    enum class Part {
        None,
        Body, ///< inside the active mask: move
        Corner, ///< index 0..3: top-left, top-right, bottom-right, bottom-left
        Edge, ///< index 0..3: top, right, bottom, left
        Rotate,
        FeatherOut,
        FeatherIn,
        Point, ///< index = point
        InTangent, ///< index = point
        OutTangent, ///< index = point
        Segment, ///< index = segment start point, t = bezier parameter
        FirstPoint, ///< drawing: the first point (closes the path)
        Canvas, ///< drawing: empty picture area (adds a point / starts a stroke)
        OtherMask ///< index = mask list index
    };
    struct Hit
    {
        Part part = Part::None;
        int index = -1;
        double t = 0.0;
    };
    /// The geometry of the shown clip at the playhead.
    struct View
    {
        const ClipData* clip = nullptr;
        const MaskData* mask = nullptr; ///< the active mask, if it belongs to the clip
        int maskIndex = -1;
        int64_t local = 0; ///< clip-local frame of the playhead
        QTransform toWidget; ///< content → widget
        QTransform toContent; ///< widget → content
    };

    [[nodiscard]] std::optional<View> view() const;
    [[nodiscard]] Hit hitTest(const View& v, QPointF pos) const;
    /// Mask-local → widget mapping of @p mask at @p local.
    [[nodiscard]] static QTransform maskToWidget(
        const MaskData& mask, int64_t local, const QTransform& toWidget);
    /// Mask-local box of the active mask's handles: width × height for parametric shapes, the
    /// points' bounding box (symmetric about the origin) for point masks.
    [[nodiscard]] static QRectF handleBox(const MaskData& mask, int64_t local);
    void updateCursor(const Hit& hit);
    void paintMask(QPainter& p, const View& v, const MaskData& mask, bool active) const;
    void paintHandles(QPainter& p, const View& v) const;

    // ---- Editing ----
    /// Starts an undoable gesture labelled @p label on the active mask.
    void beginGesture(const QString& label, const View& v, const Hit& hit, QPointF pos);
    /// Replaces the active mask with @p mask (inside the open gesture) and notifies views.
    void writeMask(const MaskData& mask);
    void dragTo(QPointF pos, Qt::KeyboardModifiers mods);
    void finishGesture();
    void cancelGesture();
    void setActiveMask(quint64 maskId);
    /// One-shot edit of the active mask (its own undo step).
    void editMask(const QString& label, const std::function<void(MaskData&, int64_t)>& fn);
    void addDrawnPoint(const View& v, QPointF pos);
    void closeDrawnPath();
    void deleteSelectedPoints();
    void toggleSmooth(int index);
    void insertPointOnSegment(const Hit& hit);

    AppContext& m_ctx;
    ViewerSurface* m_surface;
    QPointer<QAction> m_masksAction;
    bool m_editing = false;
    quint64 m_clipId = 0;
    quint64 m_lastMask = 0;
    QSet<int> m_selected;

    // Gesture state (valid while m_gesture is true).
    bool m_gesture = false;
    Hit m_dragHit;
    QPointF m_pressPos;
    MaskData m_startMask; ///< the mask when the gesture began (drags are absolute from it)
    int64_t m_gestureLocal = 0;
    QTransform m_gestureToContent;
    QTransform m_gestureToWidget;
    QVector<QPointF> m_stroke; ///< freehand: the drawn points, mask-local
    bool m_moved = false;
    QPointF m_hoverPos; ///< last mouse position (drawing preview)
    bool m_hovering = false;
};

} // namespace kk
