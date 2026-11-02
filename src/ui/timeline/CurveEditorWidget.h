// SPDX-License-Identifier: GPL-3.0-or-later
// CurveEditorWidget — the curve editor lane below a clip: value-over-time curves of the clip's
// animated parameters with draggable keys and Bezier handles.
#pragma once

#include "core/KeyframeOps.h"
#include "ui/timeline/ClipLaneWidget.h"

#include <QHash>
#include <QPolygonF>
#include <QSet>

class QToolButton;

namespace kk {

/// Curve editor of one clip (spec 15.10 - 15.12).
///
/// The header column lists the animated parameters (click toggles a curve; the last one turned
/// on is the active curve whose value axis is shown) and holds the tool bar: Linear, Smooth,
/// Ease In, Ease Out, Ease In and Out, Static, Flatten, Fit. Time follows the timeline zoom;
/// every curve is scaled to its own value range (keys and handle tips, which bound the Bezier
/// curve) so curves with different units share the lane, like Resolve's curve editor.
///
/// Mouse: click / Shift / Ctrl / rubber band select keys of the shown curves; drag moves them
/// in time (whole frames, snapping as in the keyframe editor) and value (Shift locks the axis
/// that moved first); handles of selected non-linear keys can be dragged (Alt breaks the
/// tangent); double-click on a key jumps the playhead there, double-click on empty space adds
/// a key to the active curve; Ctrl+Shift+wheel zooms the value axis, middle drag pans it.
/// Thread-safety: GUI thread only.
class CurveEditorWidget : public ClipLaneWidget
{
    Q_OBJECT
public:
    CurveEditorWidget(AppContext& ctx, const TimelineLayout& layout, TimelineController* controller,
        KeyframeLanes* lanes, quint64 clipId, QWidget* parent);

    /// Default lane height in px.
    static constexpr int kDefaultHeight = 170;

    /// Paths of the curves currently shown.
    [[nodiscard]] QVector<QString> shownPaths() const;
    /// Shows or hides the curve of @p path.
    void setCurveShown(const QString& path, bool shown);
    /// Widget-space position of the key of @p path at clip-local @p localFrame (tests).
    [[nodiscard]] QPointF keyPos(const QString& path, int64_t localFrame) const;
    /// Widget-space position of a handle tip of that key (tests).
    [[nodiscard]] QPointF handlePos(
        const QString& path, int64_t localFrame, keyframeops::HandleSide side) const;
    /// Resets the value zoom so every shown curve fits.
    void fitView();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct Range
    {
        double lo = 0.0;
        double hi = 1.0;
    };
    struct Hit
    {
        QString path;
        std::optional<int64_t> frame; ///< key hit
        std::optional<keyframeops::HandleSide> handle; ///< handle tip of that key hit
        int listRow = -1; ///< parameter list row in the header column
    };

    [[nodiscard]] QVector<keyframeops::AnimatedParam> params() const;
    [[nodiscard]] Range rangeOf(const QString& path) const;
    [[nodiscard]] QRectF graphRect() const; ///< logical coordinates
    [[nodiscard]] double valueToY(const Range& r, double value) const; ///< logical y
    [[nodiscard]] double yToValue(const Range& r, double logicalY) const;
    [[nodiscard]] Hit hitTest(QPointF widgetPos) const;
    [[nodiscard]] bool handleVisible(const AnimatedValue& v, qsizetype index, bool out) const;
    [[nodiscard]] QColor curveColor(int index) const;
    void layoutButtons();
    void addKeyAt(QPointF widgetPos);

    enum class Mode {
        None,
        Pending,
        MovePoints,
        MoveHandle,
        RubberBand,
        PanValue
    };
    Mode m_mode = Mode::None;
    QPointF m_pressPos;
    Qt::KeyboardModifiers m_pressModifiers;
    int64_t m_pressFrame = 0;
    QString m_pressPath;
    keyframeops::HandleSide m_pressSide = keyframeops::HandleSide::Out;
    int m_axisLock = 0; ///< Shift drag: 0 undecided, 1 time only, 2 value only
    QVector<KeyframeRef> m_dragKeys;
    QHash<QString, Range> m_dragRanges; ///< value ranges frozen at drag start
    QVector<KeyframeRef> m_baseSelection;
    QRectF m_band; ///< widget coordinates
    double m_panAtPress = 0.0;

    QSet<QString> m_hidden; ///< curves switched off (new parameters are shown)
    QString m_active;
    double m_zoomY = 1.0; ///< value zoom factor (1 = fit)
    double m_panY = 0.0; ///< value pan in fractions of the graph height
    QPolygonF m_polyline; ///< reused curve buffer
    QVector<QToolButton*> m_buttons;
};

} // namespace kk
