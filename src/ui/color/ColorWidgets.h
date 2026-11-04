// SPDX-License-Identifier: GPL-3.0-or-later
// ColorWidgets — the Primaries and Curves palettes of the Color page: colour wheels (balance
// puck + master strip), primary bars, the Primaries palette (wheels or bars, the numeric
// fields and the adjustment rows) and the curve editor with its Curves palette. They edit the
// grade only through ColorSession (docs/color-page.md).
#pragma once

#include "core/Grade.h"

#include <QPointer>
#include <QWidget>

#include <array>

class QButtonGroup;
class QStackedWidget;
class QToolButton;

namespace kk {

class ColorSession;
class ValueField;

/// One colour wheel: a hue disc with a balance puck (drag to tint) and a master strip below
/// it (drag left / right). Values are the deltas of the wheel's R, G, B and Y parameters from
/// neutral: the puck adds a luma-neutral colour offset (moving towards a hue adds that hue and
/// takes its opposite away, luma unchanged), the strip moves the master. The reset arrow at the
/// top right (Resolve's) or a double-click resets the wheel (a double-click on the strip: only
/// the master). Shift while dragging: fine control.
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
    /// The disc and the master strip (widget pixels; tests).
    [[nodiscard]] QRectF discRect() const;
    [[nodiscard]] QRectF stripRect() const;
    /// The reset button at the top right (a click resets the wheel, as in Resolve).
    [[nodiscard]] QRectF resetRect() const;
    /// Mouse travel (pixels) of a full puck throw and of the master strip's -range() ..
    /// +range(): the drag sensitivity, at least a fixed distance on small wheels.
    [[nodiscard]] double puckThrowPx() const;
    [[nodiscard]] double masterThrowPx() const;

    [[nodiscard]] QSize sizeHint() const override { return {120, 140}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {70, 90}; }

Q_SIGNALS:
    /// A drag started / ended (one undo step in between).
    void dragStarted();
    void dragFinished();
    /// The puck moved: new R, G, B deltas.
    void balanceChanged(const std::array<double, 3>& rgbDelta);
    /// The master strip moved: new master delta.
    void masterChanged(double masterDelta);
    /// The reset arrow or a double-click on the disc: back to neutral.
    void resetRequested();
    /// Double-click on the strip: master back to neutral.
    void masterResetRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    gradekeys::Wheel m_wheel;
    std::array<double, 3> m_rgb {};
    double m_master = 0.0;
    enum class Drag {
        None,
        Puck,
        Strip
    } m_drag = Drag::None;
    QPointF m_pressPos;
    QPointF m_pressPuck;
    std::array<double, 3> m_pressRgb {};
    double m_pressMaster = 0.0;
};

/// The bars view of one wheel: Y, R, G and B as vertical bars (Primaries > Bars). Drag a bar up
/// or down; double-click resets it.
class PrimaryBars : public QWidget
{
    Q_OBJECT
public:
    PrimaryBars(gradekeys::Wheel wheel, QWidget* parent = nullptr);

    /// Shows the channel deltas (0 R, 1 G, 2 B, 3 Y).
    void setValues(const std::array<double, 4>& deltas);
    /// Bar rectangle of @p channel (tests).
    [[nodiscard]] QRectF barRect(int channel) const;
    [[nodiscard]] double range() const;

    [[nodiscard]] QSize sizeHint() const override { return {120, 140}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {70, 90}; }

Q_SIGNALS:
    void dragStarted();
    void dragFinished();
    void channelChanged(int channel, double delta);
    void channelResetRequested(int channel);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] int channelAt(QPointF pos) const;

    gradekeys::Wheel m_wheel;
    std::array<double, 4> m_values {};
    int m_drag = -1;
    double m_pressY = 0.0;
    double m_pressValue = 0.0;
};

/// The Primaries palette: Lift, Gamma, Gain and Offset as wheels or bars, each with its Y R G
/// B fields, and the adjustment rows (Temp, Tint, Contrast, Pivot above; Mix, Saturation, Hue,
/// Lum Mix below), bound to the session's selected node at the playhead.
class PrimariesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit PrimariesPanel(ColorSession* session, QWidget* parent = nullptr);

    [[nodiscard]] ColorWheel* wheel(gradekeys::Wheel w) const { return m_wheels[size_t(w)]; }
    [[nodiscard]] PrimaryBars* bars(gradekeys::Wheel w) const { return m_bars[size_t(w)]; }
    /// The field of an adjustment key (Contrast, Pivot, ...) or of a wheel channel key
    /// (liftR, gainY, ...), or nullptr.
    [[nodiscard]] ValueField* field(const QString& key) const;
    /// Wheels (false) or bars (true).
    void setBarsMode(bool bars);
    [[nodiscard]] bool barsMode() const;
    /// Resets every primary of the selected node (the palette's reset button).
    void resetAll();

private:
    void refresh();
    /// Binds @p field to @p key: drags are one undo step, typed values one each.
    void bindField(ValueField* field, const QString& key);
    void resetWheel(gradekeys::Wheel w);

    QPointer<ColorSession> m_session;
    std::array<ColorWheel*, 4> m_wheels {};
    std::array<PrimaryBars*, 4> m_bars {};
    QStackedWidget* m_modes = nullptr;
    QToolButton* m_wheelsButton = nullptr;
    QToolButton* m_barsButton = nullptr;
    QVector<std::pair<QString, ValueField*>> m_fields;
    bool m_refreshing = false;
};

/// Curve editor for the selected node: one curve (custom Y, R, G, B or one of the HSL curves)
/// in a plot. Click adds a point, drag moves it, right-click or double-click removes it (one
/// undo step per gesture). The custom curves show the other channels faintly; with linked
/// editing a change of R, G or B applies to all three.
class GradeCurveEditor : public QWidget
{
    Q_OBJECT
public:
    explicit GradeCurveEditor(ColorSession* session, QWidget* parent = nullptr);

    [[nodiscard]] GradeCurve curve() const { return m_curve; }
    void setCurve(GradeCurve curve);
    /// Linked R, G, B editing (Custom curves).
    void setLinked(bool linked) { m_linked = linked; }
    [[nodiscard]] bool isLinked() const { return m_linked; }
    /// The plot rectangle (widget pixels; x 0..1 left to right, y 0..1 bottom to top).
    [[nodiscard]] QRectF plotRect() const;

    [[nodiscard]] QSize sizeHint() const override { return {260, 220}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {120, 90}; }

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
    GradeCurve m_curve = GradeCurve::Luma;
    bool m_linked = true;
    int m_dragIndex = -1;
};

/// The Curves palette: mode buttons (Custom, Hue vs Hue, Hue vs Sat, Hue vs Lum, Lum vs Sat,
/// Sat vs Sat), for Custom the Y / R / G / B channel buttons and the link toggle, and the
/// editor; resetCurve() is the palette bar's Reset.
class ColorCurvesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ColorCurvesPanel(ColorSession* session, QWidget* parent = nullptr);

    [[nodiscard]] GradeCurveEditor* editor() const { return m_editor; }
    /// Shows curve @p curve (selects the matching mode and channel button).
    void showCurve(GradeCurve curve);
    /// Resets the shown curve (Custom with link: R, G and B together).
    void resetCurve();

private:
    void updateButtons();

    QPointer<ColorSession> m_session;
    GradeCurveEditor* m_editor = nullptr;
    QButtonGroup* m_modes = nullptr;
    QButtonGroup* m_channels = nullptr;
    QWidget* m_channelRow = nullptr;
    QToolButton* m_link = nullptr;
};

} // namespace kk
