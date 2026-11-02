// SPDX-License-Identifier: GPL-3.0-or-later
// CurveView — value-over-time curves of keyframed parameters with draggable keys and Bezier
// handles. The inline curve lane and the Keyframes panel's curve editor are both this view.
#pragma once

#include "core/KeyframeOps.h"
#include "ui/keyframes/KeyframeViewBase.h"

#include <QPolygonF>

#include <functional>
#include <optional>

namespace kk {

/// One curve of a CurveView.
struct CurveInfo
{
    ParamId id;
    QString label; ///< parameter label ("Position X")
    QString groupName; ///< inspector group ("Transform")
    double min = 0.0; ///< descriptor range: dragged values are clamped to it
    double max = 1.0;
    int colorIndex = 0; ///< index into CurveView::curveColor()
};

/// Curve editor (spec 15.10 - 15.12 and the Keyframes panel, spec 15.14).
///
/// Value axis: in Normalised mode every curve is scaled to its own value range (keys and
/// handle tips, which bound the Bezier curve) so curves with different units share the view,
/// and the axis shows the active curve's units; in Absolute mode all curves share one value
/// axis. On top of that the view has its own value zoom and pan (fitView() resets them).
/// Time comes from the KeyframeTimeMap.
///
/// Mouse: click / Shift / Ctrl / rubber band select keys of the shown curves; drag moves them
/// in time (whole frames, snapping like the dope sheet) and value (Shift locks the axis that
/// moved first); handles of selected non-linear keys can be dragged (the opposite handle stays
/// collinear, Alt breaks the tangent); double-click on a key jumps the playhead there,
/// double-click on empty space adds a key to the active curve; Ctrl+Shift+wheel zooms the
/// value axis around the pointer, middle drag pans it (and time, when a time panner is set).
/// Other wheel events are ignored so the owner (timeline or panel) scrolls / zooms.
/// Thread-safety: GUI thread only.
class CurveView : public KeyframeViewBase
{
    Q_OBJECT
public:
    /// How values map to y (see the class comment).
    enum class AxisMode {
        Normalised,
        Absolute
    };

    CurveView(AppContext& ctx, const KeyframeTimeMap& map, QWidget* parent);

    /// Replaces the curve list (shown / hidden state and the active curve are kept by id; new
    /// curves are shown).
    void setCurves(const QVector<CurveInfo>& curves);
    [[nodiscard]] const QVector<CurveInfo>& curves() const { return m_curves; }
    /// Ids of the curves currently shown, in list order.
    [[nodiscard]] QVector<ParamId> shownCurves() const;
    /// Shows or hides a curve (showing makes it the active one).
    void setCurveShown(const ParamId& id, bool shown);
    [[nodiscard]] bool isCurveShown(const ParamId& id) const;
    /// The active curve (value axis in Normalised mode, target of double-click adds).
    [[nodiscard]] ParamId activeCurve() const { return m_active; }
    void setActiveCurve(const ParamId& id);

    void setAxisMode(AxisMode mode);
    [[nodiscard]] AxisMode axisMode() const { return m_axisMode; }
    /// Draws the value labels in the label column (panel) instead of inside the graph (lanes).
    void setAxisLabelsInColumn(bool inColumn);
    /// Draws a vertical time grid (seconds / frames) behind the curves.
    void setTimeGrid(bool on);
    /// Called with the horizontal pixel delta of a middle-button drag (pans time).
    void setTimePanner(std::function<void(double dxPx)> panner)
    {
        m_timePanner = std::move(panner);
    }

    /// Widget-space position of the key of @p id at clip-local @p localFrame (tests).
    [[nodiscard]] QPointF keyPosAt(const ParamId& id, int64_t localFrame) const;
    /// Widget-space position of a handle tip of that key (tests).
    [[nodiscard]] QPointF handlePosAt(
        const ParamId& id, int64_t localFrame, keyframeops::HandleSide side) const;

    /// Resets the value zoom so every shown curve fits.
    void fitView();
    /// Zooms the value axis to the selected keys and asks the owner to fit time to them.
    void fitSelection();
    /// Multiplies the value zoom by @p factor keeping widget y @p anchorY fixed.
    void zoomValue(double factor, double anchorY);
    /// Pans the value axis by @p dyPx widget pixels (positive: the curves move down).
    void panValue(double dyPx);
    /// Value zoom factor (1 = fit) and pan (fractions of the graph height), for persistence.
    [[nodiscard]] double valueZoom() const { return m_zoomY; }

    /// Own curve palette: distinct mid-saturation hues readable on dark and light backgrounds.
    [[nodiscard]] static QColor curveColor(int index);

signals:
    /// fitSelection(): the owner should show timeline frames [@p first, @p last].
    void fitTimeRequested(qint64 first, qint64 last);
    /// The shown / active curves changed (by a click in the view or setCurveShown()).
    void curvesChanged();

protected:
    /// Paints the label column (x < labelWidth()) after the background (inline: list + tools).
    virtual void paintLabelColumn(QPainter& p) { Q_UNUSED(p); }
    /// Left press in the label column. Return true if handled.
    virtual bool pressLabelColumn(QPointF pos)
    {
        Q_UNUSED(pos);
        return false;
    }
    /// The curve shown at @p index of curves() is hidden.
    [[nodiscard]] bool isHiddenIndex(int index) const;

    void paintEvent(QPaintEvent* event) override;
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
        ParamId id;
        std::optional<int64_t> frame; ///< key hit (clip-local)
        std::optional<keyframeops::HandleSide> handle; ///< handle tip of that key hit
    };

    [[nodiscard]] const CurveInfo* info(const ParamId& id) const;
    [[nodiscard]] const AnimatedValue* valueOf(const ParamId& id, const ClipData** clip) const;
    [[nodiscard]] Range fittedRange(const ParamId& id) const; ///< keys + handle tips, padded
    [[nodiscard]] Range rangeOf(const ParamId& id) const; ///< base range of the axis mode
    [[nodiscard]] Range absoluteFit() const;
    [[nodiscard]] QRectF graphRect() const; ///< logical coordinates
    [[nodiscard]] double valueToY(const Range& r, double value) const; ///< logical y
    [[nodiscard]] double yToValue(const Range& r, double logicalY) const;
    [[nodiscard]] Hit hitTest(QPointF widgetPos) const;
    [[nodiscard]] static bool handleVisible(const AnimatedValue& v, qsizetype index, bool out);
    void ensureActive();
    void paintGrid(QPainter& p, const QRectF& g) const;
    void addKeyAt(QPointF widgetPos);

    QVector<CurveInfo> m_curves;
    QVector<ParamId> m_hidden; ///< curves switched off (new curves are shown)
    ParamId m_active;
    AxisMode m_axisMode = AxisMode::Normalised;
    bool m_labelsInColumn = false;
    bool m_timeGrid = false;
    std::optional<Range> m_absolute; ///< Absolute mode base range (refitted by fitView())
    std::function<void(double)> m_timePanner;

    enum class Mode {
        None,
        Pending,
        MovePoints,
        MoveHandle,
        RubberBand,
        Pan
    };
    Mode m_mode = Mode::None;
    QPointF m_pressPos;
    QPointF m_lastPanPos;
    Qt::KeyboardModifiers m_pressModifiers;
    int64_t m_pressLocal = 0; ///< clip-local frame of the grabbed key
    ParamId m_pressId;
    keyframeops::HandleSide m_pressSide = keyframeops::HandleSide::Out;
    int m_axisLock = 0; ///< Shift drag: 0 undecided, 1 time only, 2 value only
    QVector<KeyframeRef> m_dragKeys;
    QVector<std::pair<ParamId, Range>> m_dragRanges; ///< value ranges frozen at drag start
    QVector<KeyframeRef> m_baseSelection;
    QRectF m_band; ///< widget coordinates
    double m_panAtPress = 0.0;

    double m_zoomY = 1.0; ///< value zoom factor (1 = fit)
    double m_panY = 0.0; ///< value pan in fractions of the base range
    QPolygonF m_polyline; ///< reused curve buffer
};

} // namespace kk
