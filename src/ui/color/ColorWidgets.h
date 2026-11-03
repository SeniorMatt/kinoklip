// SPDX-License-Identifier: GPL-3.0-or-later
// ColorWidgets — the basic controls of the minimal Color page: a colour wheel (balance puck +
// master strip), the Primaries panel (four wheels and the Contrast / Pivot / Saturation / Hue /
// Temperature / Tint / Lum Mix fields), the custom and HSL curve editor, and the node list.
// They edit the grade only through ColorSession; the full page (node editor, Resolve layout)
// replaces or extends them later (docs/color-page.md, "Page").
#pragma once

#include "core/Grade.h"

#include <QListWidget>
#include <QPointer>
#include <QWidget>

#include <array>

class QComboBox;
class QDoubleSpinBox;
class QLabel;

namespace kk {

class ColorSession;

/// One colour wheel: a hue disc with a balance puck (drag to tint) and a master strip below
/// it (drag left / right). Values are the deltas of the wheel's R, G, B and Y parameters from
/// neutral: the puck adds a luma-neutral colour offset (moving towards a hue adds that hue and
/// takes its opposite away, luma unchanged), the strip moves all three channels. Double-click
/// resets the wheel.
class ColorWheel : public QWidget
{
    Q_OBJECT
public:
    ColorWheel(gradekeys::Wheel wheel, QWidget* parent = nullptr);

    [[nodiscard]] gradekeys::Wheel wheel() const { return m_wheel; }
    /// Shows @p rgbDelta (R, G, B deltas from neutral) and @p masterDelta.
    void setValues(const std::array<double, 3>& rgbDelta, double masterDelta);
    /// The puck position for @p rgbDelta (unit disc, x = Cb, y = Cr up) and back; exposed for
    /// tests. The disc's edge is a delta of range() in the strongest channel direction.
    [[nodiscard]] QPointF puckFor(const std::array<double, 3>& rgbDelta) const;
    [[nodiscard]] std::array<double, 3> deltaFor(QPointF puck) const;
    /// Delta of a full puck throw / of the strip's full width.
    [[nodiscard]] double range() const;

    [[nodiscard]] QSize sizeHint() const override { return {130, 160}; }

Q_SIGNALS:
    /// A drag started / ended (one undo step in between).
    void dragStarted();
    void dragFinished();
    /// The puck moved: new R, G, B deltas.
    void balanceChanged(const std::array<double, 3>& rgbDelta);
    /// The master strip moved: new master delta.
    void masterChanged(double masterDelta);
    /// Double-click: back to neutral.
    void resetRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRectF discRect() const;
    [[nodiscard]] QRectF stripRect() const;

    gradekeys::Wheel m_wheel;
    std::array<double, 3> m_rgb {};
    double m_master = 0.0;
    enum class Drag {
        None,
        Puck,
        Strip
    } m_drag = Drag::None;
    QPointF m_pressPos;
    double m_pressMaster = 0.0;
};

/// The Primaries palette: Lift, Gamma, Gain and Offset wheels plus the numeric fields, bound to
/// the session's selected node at the playhead.
class PrimariesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit PrimariesPanel(ColorSession* session, QWidget* parent = nullptr);

    [[nodiscard]] ColorWheel* wheel(gradekeys::Wheel w) const { return m_wheels[size_t(w)]; }
    /// The numeric field of @p key (Contrast, Pivot, ...), or nullptr.
    [[nodiscard]] QDoubleSpinBox* field(const QString& key) const;

private:
    void refresh();
    void setWheel(gradekeys::Wheel w, const std::array<double, 3>& rgb, double master);

    QPointer<ColorSession> m_session;
    std::array<ColorWheel*, 4> m_wheels {};
    QVector<std::pair<QString, QDoubleSpinBox*>> m_fields;
    bool m_refreshing = false;
};

/// Curve editor for the selected node: a curve picker (Custom Y, R, G, B, Hue vs Hue, Hue vs
/// Sat, Hue vs Lum, Lum vs Sat, Sat vs Sat) and a plot. Click adds a point, drag moves it,
/// right-click or double-click removes it (one undo step per gesture).
class GradeCurveEditor : public QWidget
{
    Q_OBJECT
public:
    explicit GradeCurveEditor(ColorSession* session, QWidget* parent = nullptr);

    [[nodiscard]] GradeCurve curve() const { return m_curve; }
    void setCurve(GradeCurve curve);
    /// The plot rectangle (widget pixels; x 0..1 left to right, y 0..1 bottom to top).
    [[nodiscard]] QRectF plotRect() const;

    [[nodiscard]] QSize sizeHint() const override { return {260, 220}; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] CurvePoints points() const;
    [[nodiscard]] QPointF toPlot(QPointF value) const;
    [[nodiscard]] QPointF fromPlot(QPointF widget) const;
    [[nodiscard]] int pointAt(QPointF widget) const;
    void store(const CurvePoints& points);

    QPointer<ColorSession> m_session;
    QComboBox* m_picker = nullptr;
    GradeCurve m_curve = GradeCurve::Luma;
    int m_dragIndex = -1;
};

/// The node list of the minimal page: the nodes in evaluation order ("01", "02 Parallel",
/// ...) with a check box each (unchecked = bypassed). Selecting a row selects the node.
class NodeList : public QListWidget
{
    Q_OBJECT
public:
    explicit NodeList(ColorSession* session, QWidget* parent = nullptr);

private:
    void refresh();

    QPointer<ColorSession> m_session;
    bool m_refreshing = false;
};

} // namespace kk
