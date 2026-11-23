// SPDX-License-Identifier: GPL-3.0-or-later
// ColorViewerOverlay — the Color page viewer's on-screen controls: power window outlines and
// handles of the selected node, the qualifier eyedropper, the highlight picture and the split
// wipe against a gallery still.
#pragma once

#include "core/ColorMatch.h"
#include "core/Grade.h"

#include <QImage>
#include <QPointer>
#include <QTransform>
#include <QWidget>

#include <optional>

namespace kk {

class ColorSession;
class Project;
class ViewerSurface;

/// Transparent widget over the viewer surface (a child of it, like the Compositing page's
/// overlay), drawn in widget pixels.
///
/// **Coordinates.** Windows are fractions of the graded picture: the clip's content for a clip
/// grade (mapped through the clip's own transform, render::clipTransform(), so the outline
/// sits on the moved / scaled clip), the timeline frame for the timeline grade. The eyedropper
/// and the wipe work in fractions of the program frame (what the viewer shows).
///
/// **Window controls** (while setWindowControls(true): the page turns them on with the
/// Windows palette, like Resolve's on-screen controls): every window of the selected node is
/// outlined (its soft edge dashed), the selected one with handles: drag inside to move, the
/// side handles for width / height, the corner for both, the knob above for rotation, the
/// outer handle on the left for the outside softness; polygon and curve points drag one by
/// one, double-click on an edge inserts a point, right-click on a point deletes it (at least
/// three stay). A curve window's selected point shows its two tangent handles: dragging one
/// turns it (a smooth point keeps the other collinear, Alt breaks that), double-click on a
/// curve point switches it between smooth (automatic tangents) and corner (no tangents). With
/// shape keyframes every edit writes a key at the playhead. Clicking another window's outline selects it. Every drag is one undo step.
///
/// **Chart outline** (setChart(), the Color Match palette): the colour chart's four-sided
/// outline with its 6 x 4 patch grid in fractions of the program frame; drag a corner handle to
/// fit it onto the chart in the picture (a tilted or turned chart works), drag inside to move it.
/// chartQuadChanged() reports every change.
///
/// **Eyedropper** (setPickMode()): click or drag a rectangle; pickRequested() carries the
/// frame-fraction rectangle (a point for a click).
///
/// Thread-safety: GUI thread only.
class ColorViewerOverlay : public QWidget
{
    Q_OBJECT
public:
    enum class Handle {
        None,
        Move,
        Width,
        Height,
        Corner,
        Rotate,
        Soft,
        Point, ///< a polygon / curve point (index: pointIndex)
        TangentIn, ///< the incoming handle of curve point pointIndex
        TangentOut ///< the outgoing handle of curve point pointIndex
    };
    struct HandleHit
    {
        Handle handle = Handle::None;
        quint64 window = 0;
        int pointIndex = -1;
    };

    ColorViewerOverlay(ColorSession* session, Project* project, ViewerSurface* surface);

    void setWindowControls(bool on);
    [[nodiscard]] bool windowControls() const { return m_windowControls; }
    void setPickMode(bool on);
    [[nodiscard]] bool pickMode() const { return m_pick; }
    /// Picture drawn over the whole viewer picture (highlight); null = none.
    void setHighlightImage(const QImage& image);
    [[nodiscard]] const QImage& highlightImage() const { return m_highlight; }
    /// Still shown left of the wipe line; null = no wipe.
    void setWipeImage(const QImage& image);
    [[nodiscard]] bool isWiping() const { return !m_wipe.isNull(); }
    /// Wipe position: fraction of the picture width (0.5 = the middle).
    [[nodiscard]] double wipePosition() const { return m_wipePos; }
    void setWipePosition(double fraction);

    /// Shows the chart outline @p quad (fractions of the frame); @p on false hides it.
    void setChart(bool on, const colormatch::ChartQuad& quad);
    [[nodiscard]] bool chartVisible() const { return m_chartOn; }
    [[nodiscard]] const colormatch::ChartQuad& chartQuad() const { return m_chartQuad; }
    /// Widget position of chart corner @p index (0 top left, clockwise) and its hit radius.
    [[nodiscard]] QPointF chartCornerPos(int index) const;
    static constexpr double kChartHandleRadius = 9.0;

    /// Graded picture pixels (origin top-left) -> widget, and the picture's size in pixels;
    /// nullopt when there is nothing to grade.
    [[nodiscard]] std::optional<QTransform> pictureToWidget(QSizeF* pictureSize = nullptr) const;
    /// Window-local pixels (centre origin, unrotated) -> widget for @p window at the grade frame.
    [[nodiscard]] std::optional<QTransform> windowToWidget(const GradeWindow& window) const;
    /// @p w as the picture shows it at the playhead: moved by the node's tracker data. The
    /// handle functions below take shown windows.
    [[nodiscard]] GradeWindow shown(const GradeWindow& w) const;
    /// Widget position of @p handle of @p window (null point when it has none).
    [[nodiscard]] QPointF handlePos(const GradeWindow& window, Handle handle, int point = -1) const;
    [[nodiscard]] HandleHit hitTest(QPointF pos) const;
    /// The point whose tangent handles are shown on the selected curve window.
    [[nodiscard]] int selectedPoint() const { return m_selPoint; }
    /// The viewer's picture rectangle (widget pixels).
    [[nodiscard]] QRectF pictureRect() const;

Q_SIGNALS:
    /// The eyedropper picked @p frameRect (fractions of the program frame; zero size = a point).
    void pickRequested(const QRectF& frameRect);
    /// The user dragged the chart outline.
    void chartQuadChanged(const colormatch::ChartQuad& quad);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();
    void paintWindow(QPainter& p, const GradeWindow& w, bool selected) const;
    [[nodiscard]] QPainterPath windowPath(const GradeWindow& w, double grow) const;
    [[nodiscard]] QPointF toFrameFraction(QPointF widget) const;
    void applyDrag(QPointF pos);
    void paintChart(QPainter& p) const;
    /// Corner under @p pos (0..3), 4 inside the outline, -1 elsewhere.
    [[nodiscard]] int chartHit(QPointF pos) const;
    /// Sets window geometry keys given as shown (the drag works on the shown window); the
    /// tracker data is taken out again before they are stored.
    bool setShownParams(const QVector<std::pair<QString, double>>& values, const QString& label);

    QPointer<ColorSession> m_session;
    Project* m_project = nullptr;
    ViewerSurface* m_surface = nullptr;
    bool m_windowControls = false;
    bool m_pick = false;
    bool m_chartOn = false;
    colormatch::ChartQuad m_chartQuad;
    colormatch::ChartQuad m_chartPressQuad;
    int m_chartDrag = -1; ///< dragged corner 0..3, 4 = the whole outline, -1 none
    QImage m_highlight;
    QImage m_wipe;
    double m_wipePos = 0.5;

    // Drag state.
    HandleHit m_drag;
    bool m_dragWipe = false;
    int m_selPoint = 0;
    bool m_picking = false;
    QPointF m_pressPos;
    QPointF m_mousePos;
    GradeWindow m_pressWindow; ///< the window as it was at the press
    QTransform m_pressToWidget; ///< picture pixels -> widget at the press
    QSizeF m_pressPicture;
};

} // namespace kk
