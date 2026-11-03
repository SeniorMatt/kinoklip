// SPDX-License-Identifier: GPL-3.0-or-later
// ColorWidgets — wheels, primaries fields, curve editor and node list of the minimal Color page.
#include "ui/color/ColorWidgets.h"

#include "core/GradeMath.h"
#include "ui/color/ColorSession.h"

#include <QComboBox>
#include <QConicalGradient>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

namespace gk = gradekeys;
using gk::Wheel;

constexpr double kCbScale = 1.8556;
constexpr double kCrScale = 1.5748;
/// Strip height under each wheel (px).
constexpr double kStripHeight = 14.0;
/// Pick radius of curve points (px).
constexpr double kPointRadius = 6.0;

QString wheelName(Wheel w)
{
    switch (w) {
    case Wheel::Lift:
        return ColorWheel::tr("Lift");
    case Wheel::Gamma:
        return ColorWheel::tr("Gamma");
    case Wheel::Gain:
        return ColorWheel::tr("Gain");
    case Wheel::Offset:
        return ColorWheel::tr("Offset");
    }
    return {};
}

double neutralOf(Wheel w)
{
    return w == Wheel::Gain ? 1.0 : 0.0;
}

} // namespace

// ---- ColorWheel ----

ColorWheel::ColorWheel(Wheel wheel, QWidget* parent)
    : QWidget(parent)
    , m_wheel(wheel)
{
    setObjectName(QStringLiteral("ColorWheel_") + wheelName(wheel));
    setMinimumSize(90, 110);
    setToolTip(tr("%1: drag the centre to balance the colour, drag the bar below for the master "
                  "level; double-click resets")
                   .arg(wheelName(wheel)));
}

double ColorWheel::range() const
{
    return m_wheel == Wheel::Gain ? 0.5 : 0.25;
}

QPointF ColorWheel::puckFor(const std::array<double, 3>& d) const
{
    const double y = grademath::kLumaR * d[0] + grademath::kLumaG * d[1] + grademath::kLumaB * d[2];
    const double cb = (d[2] - y) / kCbScale;
    const double cr = (d[0] - y) / kCrScale;
    return QPointF(cb, cr) / range();
}

std::array<double, 3> ColorWheel::deltaFor(QPointF puck) const
{
    // Luma-neutral: red and blue from Cr / Cb, green balancing the luma.
    const double cb = puck.x() * range();
    const double cr = puck.y() * range();
    const double r = cr * kCrScale;
    const double b = cb * kCbScale;
    const double g = -(grademath::kLumaR * r + grademath::kLumaB * b) / grademath::kLumaG;
    return {r, g, b};
}

void ColorWheel::setValues(const std::array<double, 3>& rgbDelta, double masterDelta)
{
    m_rgb = rgbDelta;
    m_master = masterDelta;
    update();
}

QRectF ColorWheel::discRect() const
{
    const double top = 16;
    const double side
        = std::max(10.0, std::min(double(width()) - 8, height() - top - kStripHeight - 10));
    return QRectF((width() - side) / 2, top, side, side);
}

QRectF ColorWheel::stripRect() const
{
    const QRectF d = discRect();
    return QRectF(4, d.bottom() + 6, width() - 8, kStripHeight);
}

void ColorWheel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(QRectF(0, 0, width(), 14), Qt::AlignHCenter | Qt::AlignVCenter, wheelName(m_wheel));
    const QRectF d = discRect();
    // Hue ring: Cr up, Cb right, so a hue sits where the puck adds it.
    QConicalGradient ring(d.center(), 0);
    for (int i = 0; i <= 12; ++i) {
        const double a = i / 12.0 * 2 * 3.14159265358979;
        const std::array<double, 3> c = deltaFor(QPointF(std::cos(a), std::sin(a)));
        const double k = 0.5 / std::max({std::abs(c[0]), std::abs(c[1]), std::abs(c[2]), 1e-6});
        ring.setColorAt(i / 12.0,
            QColor::fromRgbF(float(std::clamp(0.5 + c[0] * k, 0.0, 1.0)),
                float(std::clamp(0.5 + c[1] * k, 0.0, 1.0)),
                float(std::clamp(0.5 + c[2] * k, 0.0, 1.0))));
    }
    p.setPen(Qt::NoPen);
    p.setBrush(ring);
    p.drawEllipse(d);
    QRadialGradient fade(d.center(), d.width() / 2);
    fade.setColorAt(0.0, QColor(40, 40, 44, 255));
    fade.setColorAt(0.85, QColor(40, 40, 44, 200));
    fade.setColorAt(1.0, QColor(40, 40, 44, 0));
    p.setBrush(fade);
    p.drawEllipse(d.adjusted(3, 3, -3, -3));
    p.setPen(QPen(QColor(255, 255, 255, 50), 1));
    p.drawLine(QPointF(d.left() + 4, d.center().y()), QPointF(d.right() - 4, d.center().y()));
    p.drawLine(QPointF(d.center().x(), d.top() + 4), QPointF(d.center().x(), d.bottom() - 4));
    // Puck.
    QPointF puck = puckFor(m_rgb);
    const double len = std::hypot(puck.x(), puck.y());
    if (len > 1.0)
        puck /= len;
    const QPointF at = d.center() + QPointF(puck.x(), -puck.y()) * (d.width() / 2 - 4);
    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(at, 4.5, 4.5);
    // Master strip.
    const QRectF s = stripRect();
    p.setPen(QPen(QColor(255, 255, 255, 60), 1));
    p.setBrush(QColor(28, 28, 32));
    p.drawRoundedRect(s, 3, 3);
    const double t = std::clamp(m_master / range() * 0.5 + 0.5, 0.0, 1.0);
    const double x = s.left() + t * s.width();
    p.setPen(QPen(QColor(230, 230, 230), 2));
    p.drawLine(QPointF(x, s.top() + 2), QPointF(x, s.bottom() - 2));
    p.setPen(palette().color(QPalette::WindowText));
    QFont f = font();
    f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.8));
    p.setFont(f);
    p.drawText(s, Qt::AlignCenter, QString::number(neutralOf(m_wheel) + m_master, 'f', 2));
}

void ColorWheel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPointF pos = event->position();
    m_pressPos = pos;
    m_pressMaster = m_master;
    if (stripRect().adjusted(0, -4, 0, 4).contains(pos))
        m_drag = Drag::Strip;
    else if (discRect().contains(pos))
        m_drag = Drag::Puck;
    else
        return;
    emit dragStarted();
    mouseMoveEvent(event);
}

void ColorWheel::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    if (m_drag == Drag::Puck) {
        const QRectF d = discRect();
        QPointF v = (pos - d.center()) / (d.width() / 2 - 4);
        v.setY(-v.y());
        const double len = std::hypot(v.x(), v.y());
        if (len > 1.0)
            v /= len;
        // Fine control with Shift.
        if (event->modifiers() & Qt::ShiftModifier)
            v *= 0.25;
        m_rgb = deltaFor(v);
        update();
        emit balanceChanged(m_rgb);
    } else if (m_drag == Drag::Strip) {
        const double dx = pos.x() - m_pressPos.x();
        m_master = std::clamp(
            m_pressMaster + dx / stripRect().width() * 2 * range(), -range() * 4, range() * 4);
        update();
        emit masterChanged(m_master);
    }
}

void ColorWheel::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_drag == Drag::None)
        return;
    m_drag = Drag::None;
    emit dragFinished();
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent*)
{
    emit resetRequested();
}

// ---- PrimariesPanel ----

PrimariesPanel::PrimariesPanel(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("PrimariesPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* wheels = new QHBoxLayout;
    for (const Wheel w : {Wheel::Lift, Wheel::Gamma, Wheel::Gain, Wheel::Offset}) {
        auto* wheel = new ColorWheel(w, this);
        m_wheels[size_t(w)] = wheel;
        wheels->addWidget(wheel);
        const QString label = wheelName(w);
        connect(wheel, &ColorWheel::dragStarted, this, [this, label] {
            if (m_session)
                m_session->beginGesture(label);
        });
        connect(wheel, &ColorWheel::dragFinished, this, [this] {
            if (m_session)
                m_session->endGesture();
        });
        connect(wheel, &ColorWheel::balanceChanged, this,
            [this, w, label](const std::array<double, 3>& d) {
                if (!m_session)
                    return;
                const double n = neutralOf(w);
                m_session->setParams(0,
                    {{gk::wheelKey(w, 0), n + d[0]}, {gk::wheelKey(w, 1), n + d[1]},
                        {gk::wheelKey(w, 2), n + d[2]}},
                    label);
            });
        connect(wheel, &ColorWheel::masterChanged, this, [this, w, label](double m) {
            if (m_session)
                m_session->setParam(0, gk::wheelKey(w, 3), neutralOf(w) + m, label);
        });
        connect(wheel, &ColorWheel::resetRequested, this, [this, w, label] {
            if (!m_session)
                return;
            const double n = neutralOf(w);
            m_session->setParams(0,
                {{gk::wheelKey(w, 0), n}, {gk::wheelKey(w, 1), n}, {gk::wheelKey(w, 2), n},
                    {gk::wheelKey(w, 3), n}},
                tr("Reset %1").arg(label));
        });
    }
    layout->addLayout(wheels, 1);
    auto* fields = new QGridLayout;
    struct Field
    {
        const QString* key;
        double min, max, step;
        int decimals;
    };
    const Field specs[] = {{&gk::Contrast, 0, 4, 0.01, 3}, {&gk::Pivot, 0, 1, 0.005, 3},
        {&gk::Saturation, 0, 4, 0.01, 2}, {&gk::Hue, -180, 180, 1, 1},
        {&gk::Temperature, -1, 1, 0.01, 2}, {&gk::Tint, -1, 1, 0.01, 2},
        {&gk::LumMix, 0, 1, 0.01, 2}, {&gk::Mix, 0, 1, 0.01, 2}};
    int i = 0;
    for (const Field& f : specs) {
        auto* box = new QDoubleSpinBox(this);
        box->setObjectName(QStringLiteral("Primaries_") + *f.key);
        box->setRange(f.min, f.max);
        box->setSingleStep(f.step);
        box->setDecimals(f.decimals);
        box->setKeyboardTracking(false);
        const QString key = *f.key;
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, key](double v) {
            if (!m_refreshing && m_session)
                m_session->setParam(0, key, v, gk::label(key));
        });
        auto* label = new QLabel(gk::label(key), this);
        label->setBuddy(box);
        fields->addWidget(label, i / 4, (i % 4) * 2, Qt::AlignRight);
        fields->addWidget(box, i / 4, (i % 4) * 2 + 1);
        m_fields.append({key, box});
        ++i;
    }
    layout->addLayout(fields);
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &PrimariesPanel::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &PrimariesPanel::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &PrimariesPanel::refresh);
    }
    refresh();
}

QDoubleSpinBox* PrimariesPanel::field(const QString& key) const
{
    for (const auto& [k, box] : m_fields) {
        if (k == key)
            return box;
    }
    return nullptr;
}

void PrimariesPanel::refresh()
{
    m_refreshing = true;
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    const bool editable = m_session && m_session->hasGrade() && (!n || !n->isMixer());
    setEnabled(editable);
    const int64_t frame = m_session ? m_session->gradeFrame() : 0;
    auto value
        = [&](const QString& key) { return n ? n->value(key, frame) : gk::defaultValue(key); };
    for (const Wheel w : {Wheel::Lift, Wheel::Gamma, Wheel::Gain, Wheel::Offset}) {
        const double neutral = neutralOf(w);
        const std::array<double, 3> rgb = {value(gk::wheelKey(w, 0)) - neutral,
            value(gk::wheelKey(w, 1)) - neutral, value(gk::wheelKey(w, 2)) - neutral};
        m_wheels[size_t(w)]->setValues(rgb, value(gk::wheelKey(w, 3)) - neutral);
    }
    for (const auto& [key, box] : std::as_const(m_fields)) {
        const QSignalBlocker block(box);
        box->setValue(value(key));
    }
    m_refreshing = false;
}

// ---- GradeCurveEditor ----

GradeCurveEditor::GradeCurveEditor(ColorSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("GradeCurveEditor"));
    setMinimumSize(180, 160);
    m_picker = new QComboBox(this);
    m_picker->setObjectName(QStringLiteral("CurvePicker"));
    const std::pair<GradeCurve, const char*> names[]
        = {{GradeCurve::Luma, QT_TR_NOOP("Custom (Y)")}, {GradeCurve::Red, QT_TR_NOOP("Red")},
            {GradeCurve::Green, QT_TR_NOOP("Green")}, {GradeCurve::Blue, QT_TR_NOOP("Blue")},
            {GradeCurve::HueHue, QT_TR_NOOP("Hue vs Hue")},
            {GradeCurve::HueSat, QT_TR_NOOP("Hue vs Sat")},
            {GradeCurve::HueLum, QT_TR_NOOP("Hue vs Lum")},
            {GradeCurve::LumSat, QT_TR_NOOP("Lum vs Sat")},
            {GradeCurve::SatSat, QT_TR_NOOP("Sat vs Sat")}};
    for (const auto& [c, name] : names)
        m_picker->addItem(tr(name), int(c));
    m_picker->move(4, 4);
    connect(m_picker, &QComboBox::currentIndexChanged, this,
        [this](int i) { setCurve(GradeCurve(m_picker->itemData(i).toInt())); });
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, qOverload<>(&QWidget::update));
        connect(m_session, &ColorSession::selectedNodeChanged, this, qOverload<>(&QWidget::update));
    }
}

void GradeCurveEditor::setCurve(GradeCurve curve)
{
    m_curve = curve;
    const int index = m_picker->findData(int(curve));
    if (index != m_picker->currentIndex()) {
        const QSignalBlocker block(m_picker);
        m_picker->setCurrentIndex(index);
    }
    update();
}

QRectF GradeCurveEditor::plotRect() const
{
    const double top = m_picker->geometry().bottom() + 6;
    return QRectF(8, top, std::max(10.0, width() - 16.0), std::max(10.0, height() - top - 8));
}

CurvePoints GradeCurveEditor::points() const
{
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    return n ? n->curves[m_curve] : CurvePoints {};
}

QPointF GradeCurveEditor::toPlot(QPointF v) const
{
    const QRectF r = plotRect();
    return {r.left() + v.x() * r.width(), r.bottom() - v.y() * r.height()};
}

QPointF GradeCurveEditor::fromPlot(QPointF w) const
{
    const QRectF r = plotRect();
    return {std::clamp((w.x() - r.left()) / r.width(), 0.0, 1.0),
        std::clamp((r.bottom() - w.y()) / r.height(), 0.0, 1.0)};
}

int GradeCurveEditor::pointAt(QPointF w) const
{
    const CurvePoints pts = points();
    for (int i = 0; i < pts.size(); ++i) {
        const QPointF at = toPlot(QPointF(pts[i].x, pts[i].y));
        if (std::hypot(at.x() - w.x(), at.y() - w.y()) <= kPointRadius + 1)
            return i;
    }
    return -1;
}

void GradeCurveEditor::store(const CurvePoints& pts)
{
    if (m_session)
        m_session->setCurve(0, m_curve, pts, tr("Curves"));
}

void GradeCurveEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    p.fillRect(r, QColor(24, 24, 28));
    p.setPen(QPen(QColor(255, 255, 255, 30), 1));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()),
            QPointF(r.left() + r.width() * i / 4, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4),
            QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    // Hue curves: a hue band along the bottom.
    if (isHueCurve(m_curve)) {
        for (int i = 0; i < 64; ++i) {
            p.fillRect(QRectF(r.left() + r.width() * i / 64, r.bottom() - 6, r.width() / 64 + 1, 6),
                QColor::fromHsvF(float(i / 64.0), 0.8f, 0.9f));
        }
    }
    const QColor colours[]
        = {QColor(230, 230, 230), QColor(240, 80, 80), QColor(90, 220, 90), QColor(100, 140, 255)};
    const QColor colour = isChannelCurve(m_curve) ? colours[int(m_curve)] : QColor(230, 200, 120);
    const CurvePoints pts = points();
    QPainterPath path;
    for (int i = 0; i <= 128; ++i) {
        const double x = i / 128.0;
        const double y = grademath::evaluateCurve(pts, m_curve, x);
        const QPointF at = toPlot(QPointF(x, y));
        if (i == 0)
            path.moveTo(at);
        else
            path.lineTo(at);
    }
    p.setPen(QPen(colour, 1.5));
    p.drawPath(path);
    p.setBrush(QColor(30, 30, 34));
    for (const CurvePoint& pt : pts)
        p.drawEllipse(toPlot(QPointF(pt.x, pt.y)), kPointRadius * 0.7, kPointRadius * 0.7);
}

void GradeCurveEditor::mousePressEvent(QMouseEvent* event)
{
    if (!m_session || !m_session->hasGrade() || !plotRect().contains(event->position()))
        return;
    const int hit = pointAt(event->position());
    if (event->button() == Qt::RightButton) {
        if (hit >= 0) {
            CurvePoints pts = points();
            pts.remove(hit);
            store(pts);
        }
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_session->beginGesture(tr("Curves"));
    CurvePoints pts = points();
    if (hit >= 0) {
        m_dragIndex = hit;
    } else {
        const QPointF v = fromPlot(event->position());
        m_dragIndex = grademath::addCurvePoint(pts, m_curve, v.x(), v.y());
        store(pts);
    }
}

void GradeCurveEditor::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragIndex < 0 || !m_session)
        return;
    CurvePoints pts = points();
    if (m_dragIndex >= pts.size())
        return;
    const QPointF v = fromPlot(event->position());
    // Channel and lum / sat curves keep their points in x order between the neighbours.
    double x = v.x();
    if (!isHueCurve(m_curve)) {
        const double lo = m_dragIndex > 0 ? pts[m_dragIndex - 1].x + 1e-3 : 0.0;
        const double hi = m_dragIndex + 1 < pts.size() ? pts[m_dragIndex + 1].x - 1e-3 : 1.0;
        x = std::clamp(x, lo, std::max(lo, hi));
    }
    const CurvePoint moved {x, v.y()};
    pts[m_dragIndex] = moved;
    std::sort(
        pts.begin(), pts.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    m_dragIndex = int(pts.indexOf(moved));
    store(pts);
}

void GradeCurveEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_dragIndex < 0)
        return;
    m_dragIndex = -1;
    if (m_session)
        m_session->endGesture();
}

void GradeCurveEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int hit = pointAt(event->position());
    if (hit < 0)
        return;
    CurvePoints pts = points();
    pts.remove(hit);
    store(pts);
}

// ---- NodeList ----

NodeList::NodeList(ColorSession* session, QWidget* parent)
    : QListWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("ColorNodeList"));
    setMinimumWidth(120);
    connect(this, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (!m_refreshing && item && m_session)
            m_session->selectNode(item->data(Qt::UserRole).toULongLong());
    });
    connect(this, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (m_refreshing || !m_session)
            return;
        const quint64 id = item->data(Qt::UserRole).toULongLong();
        const GradeNode* n = m_session->grade().node(id);
        if (n && n->enabled != (item->checkState() == Qt::Checked))
            m_session->toggleNodeBypass(id);
    });
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &NodeList::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &NodeList::refresh);
    }
    refresh();
}

void NodeList::refresh()
{
    m_refreshing = true;
    clear();
    if (m_session) {
        const ClipGrade& g = m_session->grade();
        for (const int index : gradeops::evaluationOrder(g)) {
            const GradeNode& n = g.nodes[index];
            QString text = gradeNodeName(g, index);
            if (n.type == GradeNodeType::Parallel)
                text += QLatin1Char(' ') + tr("Parallel Mixer");
            else if (n.type == GradeNodeType::Layer)
                text += QLatin1Char(' ') + tr("Layer Mixer");
            auto* item = new QListWidgetItem(text, this);
            item->setData(Qt::UserRole, n.id);
            if (!n.isMixer()) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(n.enabled ? Qt::Checked : Qt::Unchecked);
            }
            if (n.id == m_session->selectedNode())
                setCurrentItem(item);
        }
    }
    m_refreshing = false;
}

} // namespace kk
