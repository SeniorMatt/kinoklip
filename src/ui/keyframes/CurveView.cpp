// SPDX-License-Identifier: GPL-3.0-or-later
// CurveView implementation: value mapping, grid, curve drawing, key and handle drags.
#include "ui/keyframes/CurveView.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/keyframes/KeyframeGrid.h"
#include "ui/keyframes/KeyframeTimeMap.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <cmath>
#include <limits>

namespace kk {

namespace ko = keyframeops;

namespace {
constexpr int kGraphMargin = 10; ///< vertical padding of the graph area
constexpr double kPointRadius = 3.5;
constexpr double kHitPx = 6.0;
constexpr double kSampleStepPx = 2.0; ///< curve polyline resolution
constexpr double kWheelZoomPerStep = 1.15; ///< value zoom per wheel notch (120 units)
constexpr double kMinZoom = 0.05;
constexpr double kMaxZoom = 500.0;
constexpr double kValueGridPx = 28.0; ///< about one value grid line per this many px
constexpr double kTimeGridPx = 70.0; ///< minimum distance of time grid lines
constexpr double kFitSelectionFill = 0.8; ///< selected keys fill this much of the height
} // namespace

CurveView::CurveView(AppContext& ctx, const KeyframeTimeMap& map, QWidget* parent)
    : KeyframeViewBase(ctx, map, parent)
{
    m_polyline.reserve(4096);
}

QColor CurveView::curveColor(int index)
{
    // Distinct scheme colours (negative, positive, link, ...: red, green, blue first).
    return schemeColors().series(std::max(0, index));
}

// ---- Curves ----

void CurveView::setCurves(const QVector<CurveInfo>& curves)
{
    QVector<ParamId> before;
    for (const CurveInfo& c : std::as_const(m_curves))
        before.append(c.id);
    m_curves = curves;
    QVector<ParamId> after;
    for (const CurveInfo& c : std::as_const(m_curves))
        after.append(c.id);
    if (before != after)
        m_absolute.reset(); // a different set of curves: refit the shared axis
    update();
}

const CurveInfo* CurveView::info(const ParamId& id) const
{
    for (const CurveInfo& c : m_curves) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

QVector<ParamId> CurveView::shownCurves() const
{
    QVector<ParamId> out;
    for (const CurveInfo& c : m_curves) {
        if (!m_hidden.contains(c.id))
            out.append(c.id);
    }
    return out;
}

bool CurveView::isCurveShown(const ParamId& id) const
{
    return !m_hidden.contains(id);
}

bool CurveView::isHiddenIndex(int index) const
{
    return index < 0 || index >= m_curves.size() || m_hidden.contains(m_curves[index].id);
}

void CurveView::setCurveShown(const ParamId& id, bool shown)
{
    if (shown) {
        m_hidden.removeAll(id);
        m_active = id;
    } else {
        if (!m_hidden.contains(id))
            m_hidden.append(id);
        if (m_active == id)
            m_active = {};
    }
    m_absolute.reset();
    update();
    emit curvesChanged();
}

void CurveView::setActiveCurve(const ParamId& id)
{
    if (m_active == id)
        return;
    m_active = id;
    update();
    emit curvesChanged();
}

void CurveView::ensureActive()
{
    if (!m_active.path.isEmpty() && info(m_active) && !m_hidden.contains(m_active))
        return;
    m_active = {};
    for (const CurveInfo& c : std::as_const(m_curves)) {
        if (!m_hidden.contains(c.id)) {
            m_active = c.id;
            break;
        }
    }
}

void CurveView::setAxisMode(AxisMode mode)
{
    if (mode == m_axisMode)
        return;
    m_axisMode = mode;
    fitView();
}

void CurveView::setAxisLabelsInColumn(bool inColumn)
{
    m_labelsInColumn = inColumn;
    update();
}

void CurveView::setTimeGrid(bool on)
{
    m_timeGrid = on;
    update();
}

void CurveView::fitView()
{
    m_zoomY = 1.0;
    m_panY = 0.0;
    m_absolute.reset();
    update();
}

void CurveView::zoomValue(double factor, double anchorY)
{
    // Keep the normalised value under anchorY fixed: n = (z - 0.5) / zoom + 0.5 + pan.
    const QRectF g = graphRect();
    const double z = (g.bottom() - (anchorY + yOffset())) / g.height();
    const double n = (z - 0.5) / m_zoomY + 0.5 + m_panY;
    m_zoomY = std::clamp(m_zoomY * factor, kMinZoom, kMaxZoom);
    m_panY = n - (z - 0.5) / m_zoomY - 0.5;
    update();
}

void CurveView::panValue(double dyPx)
{
    m_panY += dyPx / (graphRect().height() * m_zoomY);
    update();
}

void CurveView::fitSelection()
{
    const QVector<KeyframeRef> keys = selectedKeys();
    if (keys.isEmpty()) {
        fitView();
        return;
    }
    // Normalised positions of the selected keys in their curves' base ranges.
    double n0 = 1e300;
    double n1 = -1e300;
    int64_t f0 = std::numeric_limits<int64_t>::max();
    int64_t f1 = std::numeric_limits<int64_t>::min();
    for (const KeyframeRef& r : keys) {
        const ParamId id {r.clipId, r.paramPath};
        if (m_hidden.contains(id))
            continue;
        const ClipData* c = nullptr;
        const AnimatedValue* v = valueOf(id, &c);
        const qsizetype i = v ? v->indexOf(r.frame) : -1;
        if (i < 0)
            continue;
        const Range range = rangeOf(id);
        const double n = (v->keys[i].value - range.lo) / (range.hi - range.lo);
        n0 = std::min(n0, n);
        n1 = std::max(n1, n);
        f0 = std::min(f0, c->start + r.frame);
        f1 = std::max(f1, c->start + r.frame);
    }
    if (n0 > n1)
        return;
    const double span = std::max(n1 - n0, 0.05);
    m_zoomY = std::clamp(kFitSelectionFill / span, kMinZoom, kMaxZoom);
    m_panY = (n0 + n1) / 2.0 - 0.5;
    update();
    emit fitTimeRequested(f0, f1);
}

// ---- Geometry ----

QRectF CurveView::graphRect() const
{
    return QRectF(labelWidth(), kGraphMargin, std::max(1, width() - labelWidth()),
        std::max(10, logicalHeight() - 2 * kGraphMargin));
}

const AnimatedValue* CurveView::valueOf(const ParamId& id, const ClipData** clip) const
{
    const ClipData* c = clipData(id.clipId);
    if (clip)
        *clip = c;
    return c ? ko::findValue(*c, id.path) : nullptr;
}

CurveView::Range CurveView::fittedRange(const ParamId& id) const
{
    const AnimatedValue* v = valueOf(id, nullptr);
    if (!v)
        return {};
    double lo = v->keys.isEmpty() ? v->base : v->keys.front().value;
    double hi = lo;
    // A Bezier segment lies inside the convex hull of its control points, so the key values
    // plus the handle tips bound the whole curve: no sampling needed.
    for (qsizetype i = 0; i < v->keys.size(); ++i) {
        const double value = v->keys[i].value;
        lo = std::min(lo, value);
        hi = std::max(hi, value);
        if (i + 1 < v->keys.size() && !v->keys[i].hold) {
            const double tip = value + v->outHandle(i).second;
            const double tipIn = v->keys[i + 1].value - v->inHandle(i + 1).second;
            lo = std::min({lo, tip, tipIn});
            hi = std::max({hi, tip, tipIn});
        }
    }
    const double span = hi - lo;
    const double pad = span > 1e-9 ? span * 0.08 : std::max(0.5, std::abs(lo) * 0.1);
    return {lo - pad, hi + pad};
}

CurveView::Range CurveView::absoluteFit() const
{
    std::optional<Range> all;
    for (const CurveInfo& c : m_curves) {
        if (m_hidden.contains(c.id))
            continue;
        Range r = fittedRange(c.id);
        if (c.displayNegated)
            r = Range {-r.hi, -r.lo}; // the shared axis is in shown values
        all = all ? Range {std::min(all->lo, r.lo), std::max(all->hi, r.hi)} : r;
    }
    return all.value_or(Range {});
}

CurveView::Range CurveView::rangeOf(const ParamId& id) const
{
    // While dragging, the ranges stay as they were at the press so the view does not rescale
    // under the pointer.
    if (m_mode == Mode::MovePoints || m_mode == Mode::MoveHandle) {
        for (const auto& [rid, r] : m_dragRanges) {
            if (rid == id)
                return r;
        }
    }
    // A curve shown negated (Position Y, shown +up) maps stored values through the mirrored
    // range: the stored value v lands where the shown value -v would.
    const CurveInfo* curve = id.path.isEmpty() ? nullptr : info(id);
    const bool negated = curve && curve->displayNegated;
    if (m_axisMode == AxisMode::Absolute) {
        if (!m_absolute)
            const_cast<CurveView*>(this)->m_absolute = absoluteFit();
        return negated ? Range {-m_absolute->lo, -m_absolute->hi} : *m_absolute;
    }
    const Range r = fittedRange(id);
    return negated ? Range {r.hi, r.lo} : r;
}

double CurveView::valueToY(const Range& r, double value) const
{
    const QRectF g = graphRect();
    const double n = (value - r.lo) / (r.hi - r.lo);
    const double z = (n - 0.5 - m_panY) * m_zoomY + 0.5;
    return g.bottom() - z * g.height();
}

double CurveView::yToValue(const Range& r, double logicalY) const
{
    const QRectF g = graphRect();
    const double z = (g.bottom() - logicalY) / g.height();
    const double n = (z - 0.5) / m_zoomY + 0.5 + m_panY;
    return r.lo + n * (r.hi - r.lo);
}

bool CurveView::handleVisible(const AnimatedValue& v, qsizetype i, bool out)
{
    // Handles exist where a segment does and are only shown for curved sides (a Linear side
    // has no handle in Resolve-style editors; pick Smooth or an ease to get one).
    if (out && (i + 1 >= v.keys.size() || v.keys[i].hold))
        return false;
    if (!out && (i == 0 || v.keys[i - 1].hold))
        return false;
    return (out ? v.keys[i].out.mode : v.keys[i].in.mode) != KeyHandle::Mode::Linear;
}

QPointF CurveView::keyPosAt(const ParamId& id, int64_t localFrame) const
{
    const ClipData* c = nullptr;
    const AnimatedValue* v = valueOf(id, &c);
    const qsizetype i = v ? v->indexOf(localFrame) : -1;
    if (i < 0)
        return {};
    return QPointF(
        xOfLocal(*c, double(localFrame)), valueToY(rangeOf(id), v->keys[i].value) - yOffset());
}

QPointF CurveView::handlePosAt(const ParamId& id, int64_t localFrame, ko::HandleSide side) const
{
    const ClipData* c = nullptr;
    const AnimatedValue* v = valueOf(id, &c);
    const qsizetype i = v ? v->indexOf(localFrame) : -1;
    if (i < 0)
        return {};
    const Keyframe& k = v->keys[i];
    const Range r = rangeOf(id);
    if (side == ko::HandleSide::Out) {
        const auto [dt, dv] = v->outHandle(i);
        return QPointF(xOfLocal(*c, double(k.frame) + dt), valueToY(r, k.value + dv) - yOffset());
    }
    const auto [dt, dv] = v->inHandle(i);
    return QPointF(xOfLocal(*c, double(k.frame) - dt), valueToY(r, k.value - dv) - yOffset());
}

CurveView::Hit CurveView::hitTest(QPointF pos) const
{
    Hit hit;
    if (pos.x() < labelWidth())
        return hit;
    double best = kHitPx;
    // Handle tips of selected keys first: they sit on top of the curve.
    for (const KeyframeRef& ref : selectedKeys()) {
        const ParamId id {ref.clipId, ref.paramPath};
        if (m_hidden.contains(id) || !info(id))
            continue;
        const AnimatedValue* v = valueOf(id, nullptr);
        const qsizetype i = v ? v->indexOf(ref.frame) : -1;
        if (i < 0)
            continue;
        for (const bool out : {false, true}) {
            if (!handleVisible(*v, i, out))
                continue;
            const QPointF tip
                = handlePosAt(id, ref.frame, out ? ko::HandleSide::Out : ko::HandleSide::In);
            const double d = QLineF(tip, pos).length();
            if (d < best) {
                best = d;
                hit.id = id;
                hit.frame = ref.frame;
                hit.handle = out ? ko::HandleSide::Out : ko::HandleSide::In;
            }
        }
    }
    if (hit.handle)
        return hit;
    for (const CurveInfo& ci : m_curves) {
        if (m_hidden.contains(ci.id))
            continue;
        const ClipData* c = nullptr;
        const AnimatedValue* v = valueOf(ci.id, &c);
        if (!v)
            continue;
        const Range r = rangeOf(ci.id);
        for (const Keyframe& k : v->keys) {
            const QPointF kp(xOfLocal(*c, double(k.frame)), valueToY(r, k.value) - yOffset());
            const double d = QLineF(kp, pos).length();
            // Ties prefer the active curve.
            if (d < best || (d <= best && ci.id == m_active)) {
                best = d;
                hit.id = ci.id;
                hit.frame = k.frame;
            }
        }
    }
    return hit;
}

// ---- Painting ----

void CurveView::paintGrid(QPainter& p, const QRectF& g) const
{
    const QPalette& pal = palette();
    const QColor text = pal.color(QPalette::WindowText);
    const QColor dim = pal.color(QPalette::PlaceholderText);
    QColor grid = text;
    grid.setAlpha(28);
    const int visTop = yOffset();
    const int visBottom = yOffset() + height();

    // Time grid (panel): whole frames / seconds, at least kTimeGridPx apart.
    if (m_timeGrid) {
        const int64_t step = keyframegrid::timeStepFrames(
            m_ctx.project->timeline()->data().fps, pixelsPerFrame(), kTimeGridPx);
        const double f0 = frameAtX(g.left());
        const double f1 = frameAtX(g.right());
        QColor tgrid = text;
        tgrid.setAlpha(18);
        p.setPen(tgrid);
        for (int64_t f = int64_t(std::ceil(f0 / double(step))) * step; double(f) <= f1; f += step) {
            const double x = std::round(timeMap().frameToX(double(f))) + 0.5;
            p.drawLine(QPointF(x, visTop), QPointF(x, visBottom));
        }
    }

    // Value grid with the labels of the axis (active curve in Normalised mode).
    if (m_active.path.isEmpty() && m_axisMode == AxisMode::Normalised)
        return;
    const Range r = m_axisMode == AxisMode::Absolute ? rangeOf({}) : rangeOf(m_active);
    const double vTop = yToValue(r, g.top());
    const double vBottom = yToValue(r, g.bottom());
    const double step = keyframegrid::niceStep(
        std::abs(vTop - vBottom), std::max(2, int(g.height() / kValueGridPx)));
    const QFontMetrics fm = p.fontMetrics();
    const int lw = labelWidth();
    for (double v = std::ceil(std::min(vTop, vBottom) / step) * step;
         v <= std::max(vTop, vBottom) + step * 1e-6; v += step) {
        const double y = std::round(valueToY(r, v)) + 0.5;
        p.setPen(grid);
        p.drawLine(QPointF(g.left(), y), QPointF(g.right(), y));
        // "-0" from rounding looks odd on an axis.
        // In Normalised mode the axis is the active curve's: label its shown values.
        const double shown = r.lo > r.hi ? -v : v;
        const QString label
            = QString::number(std::abs(shown) < step * 1e-6 ? 0.0 : shown, 'g', 6);
        p.setPen(dim);
        if (m_labelsInColumn) {
            // Right-aligned in the axis column, centred on the line; skipped where it would be
            // cut by the visible area.
            const double half = fm.height() / 2.0;
            if (y - half < visTop || y + half > visBottom)
                continue;
            p.drawText(
                QRectF(0, y - half, lw - 6, fm.height()), Qt::AlignRight | Qt::AlignVCenter, label);
        } else {
            // Above the line; below it when the line is so high that the text would be cut,
            // never under the bottom edge (the old lanes clipped the lowest label).
            double baseline = y - 3;
            if (baseline - fm.ascent() < visTop)
                baseline = y + fm.ascent() + 2;
            if (baseline + fm.descent() > std::min<double>(visBottom, logicalHeight()))
                continue;
            p.drawText(QPointF(g.left() + 4, baseline), label);
        }
    }
}

void CurveView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.translate(0, -yOffset());
    paintBackground(p, QRect(0, yOffset(), width(), height()));
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() - 1.0));
    p.setFont(small);
    ensureActive();
    paintLabelColumn(p);
    p.setFont(small);

    const QPalette& pal = palette();
    const QColor text = pal.color(QPalette::WindowText);
    const QRectF g = graphRect();
    p.save();
    if (m_labelsInColumn)
        p.setClipRect(QRectF(0, yOffset(), width(), height()));
    else
        p.setClipRect(QRectF(labelWidth(), yOffset(), width() - labelWidth(), height()));
    paintGrid(p, g);
    p.setClipRect(QRectF(labelWidth(), yOffset(), width() - labelWidth(), height()));

    p.setRenderHint(QPainter::Antialiasing, true);
    const double xMin = g.left();
    const double xMax = g.right();
    // The active curve is drawn last so it is on top.
    auto drawCurve = [&](const CurveInfo& ci) {
        const ClipData* c = nullptr;
        const AnimatedValue* v = valueOf(ci.id, &c);
        if (!v || v->keys.isEmpty())
            return;
        const Range r = rangeOf(ci.id);
        const QColor col = curveColor(ci.colorIndex);
        const bool active = ci.id == m_active;
        // Curve: sampled every kSampleStepPx across the clip (and keys left outside it by a
        // trim), allocation-free after warm-up.
        m_polyline.clear();
        const double c0
            = std::max(xMin, xOfLocal(*c, double(std::min<int64_t>(0, v->keys.front().frame))));
        const double c1 = std::min(
            xMax, xOfLocal(*c, double(std::max<int64_t>(c->duration, v->keys.back().frame))));
        for (double x = c0; x <= c1 + kSampleStepPx; x += kSampleStepPx) {
            const double xc = std::min(x, c1);
            m_polyline.append(
                QPointF(xc, valueToY(r, v->valueAtTime(frameAtX(xc) - double(c->start)))));
        }
        QColor line = col;
        if (!active)
            line.setAlpha(170);
        p.setPen(QPen(line, active ? 1.6 : 1.2));
        p.drawPolyline(m_polyline);

        // Keys and the handles of selected keys.
        for (qsizetype k = 0; k < v->keys.size(); ++k) {
            const Keyframe& key = v->keys[k];
            const QPointF kp(xOfLocal(*c, double(key.frame)), valueToY(r, key.value));
            if (kp.x() < xMin - 10 || kp.x() > xMax + 10)
                continue;
            const bool selected = isSelected({ci.id.clipId, ci.id.path, key.frame});
            if (selected) {
                for (const bool out : {false, true}) {
                    if (!handleVisible(*v, k, out))
                        continue;
                    const auto [dt, dv] = out ? v->outHandle(k) : v->inHandle(k);
                    const QPointF tip(xOfLocal(*c, double(key.frame) + (out ? dt : -dt)),
                        valueToY(r, key.value + (out ? dv : -dv)));
                    p.setPen(QPen(text, 1));
                    p.drawLine(kp, tip);
                    p.setBrush(pal.color(QPalette::Base));
                    p.drawEllipse(tip, 3.0, 3.0);
                }
            }
            const bool outside = key.frame < 0 || key.frame >= c->duration;
            QColor fill = selected ? schemeColors().keyframeSelected : col;
            if (outside)
                fill.setAlpha(110);
            p.setPen(QPen(withAlpha(schemeColors().shadow, outside ? 110 : 255), 1));
            p.setBrush(fill);
            if (key.hold)
                p.drawRect(QRectF(kp.x() - kPointRadius, kp.y() - kPointRadius, 2 * kPointRadius,
                    2 * kPointRadius));
            else
                drawDiamond(p, kp, kPointRadius + 0.5);
            p.setBrush(Qt::NoBrush);
        }
    };
    const CurveInfo* activeInfo = nullptr;
    for (const CurveInfo& ci : std::as_const(m_curves)) {
        if (m_hidden.contains(ci.id))
            continue;
        if (ci.id == m_active)
            activeInfo = &ci;
        else
            drawCurve(ci);
    }
    if (activeInfo)
        drawCurve(*activeInfo);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.restore();
    paintPlayhead(p);
    p.translate(0, yOffset());
    paintBand(p, m_band);
}

// ---- Mouse ----

void CurveView::mousePressEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    m_pressPos = pos;
    m_lastPanPos = pos;
    m_pressModifiers = event->modifiers();
    m_axisLock = 0;
    if (event->button() == Qt::MiddleButton) {
        m_mode = Mode::Pan;
        m_panAtPress = m_panY;
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (pos.x() < labelWidth()) {
        pressLabelColumn(pos);
        return;
    }
    const Hit hit = hitTest(pos);
    if (hit.frame) {
        m_pressId = hit.id;
        m_pressLocal = *hit.frame;
        setActiveCurve(hit.id);
        if (hit.handle) {
            m_pressSide = *hit.handle;
            m_dragRanges = {{hit.id, rangeOf(hit.id)}};
            m_mode = Mode::MoveHandle;
            beginDragEdit(tr("Change Keyframe Handle"));
            return;
        }
        clickSelect({{hit.id.clipId, hit.id.path, *hit.frame}}, event->modifiers());
        m_mode = Mode::Pending;
        return;
    }
    m_baseSelection = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)
        ? selectedKeys()
        : QVector<KeyframeRef> {};
    setSelection(m_baseSelection);
    m_mode = Mode::RubberBand;
    m_band = QRectF(pos, QSizeF(1, 1));
    update();
}

void CurveView::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    if (m_mode == Mode::Pan) {
        m_panY = m_panAtPress + (pos.y() - m_pressPos.y()) / (graphRect().height() * m_zoomY);
        if (m_timePanner)
            m_timePanner(pos.x() - m_lastPanPos.x());
        m_lastPanPos = pos;
        update();
        return;
    }
    if (m_mode == Mode::Pending
        && (pos - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_dragKeys = selectedKeys();
        if (m_dragKeys.isEmpty()) {
            m_mode = Mode::None;
            return;
        }
        m_dragRanges.clear();
        for (const KeyframeRef& r : std::as_const(m_dragKeys)) {
            const ParamId id {r.clipId, r.paramPath};
            if (std::none_of(m_dragRanges.cbegin(), m_dragRanges.cend(),
                    [&](const auto& e) { return e.first == id; }))
                m_dragRanges.append({id, rangeOf(id)});
        }
        // Absolute mode: every curve shares the axis, freeze it for the shown ones too.
        if (m_axisMode == AxisMode::Absolute) {
            for (const CurveInfo& ci : std::as_const(m_curves))
                m_dragRanges.append({ci.id, rangeOf(ci.id)});
        }
        beginDragEdit(tr("Move Keyframes"));
        m_mode = Mode::MovePoints;
    }
    if ((m_mode == Mode::MovePoints || m_mode == Mode::MoveHandle) && !dragEditOpen()) {
        m_mode = Mode::None; // cancelled with Esc
        return;
    }
    if (m_mode == Mode::MovePoints) {
        QPointF d = pos - m_pressPos;
        if (m_pressModifiers & Qt::ShiftModifier || event->modifiers() & Qt::ShiftModifier) {
            if (m_axisLock == 0 && d.manhattanLength() >= QApplication::startDragDistance())
                m_axisLock = std::abs(d.x()) >= std::abs(d.y()) ? 1 : 2;
            if (m_axisLock == 1)
                d.setY(0);
            else if (m_axisLock == 2)
                d.setX(0);
        }
        // The grabbed key's timeline frame decides the snap; every key moves by the same delta.
        const ClipData* pressClip = editops::clipById(dragOrigin(), m_pressId.clipId);
        const int64_t pressFrame = (pressClip ? pressClip->start : 0) + m_pressLocal;
        int64_t dt = 0;
        if (d.x() != 0.0) {
            const int64_t raw
                = std::llround(frameAtX(m_pressPos.x() + d.x()) - frameAtX(m_pressPos.x()));
            dt = snapFrame(pressFrame + raw) - pressFrame;
            dt = ko::clampMoveDelta(dragOrigin(), m_dragKeys, dt);
        }
        // Value change as a fraction of the graph, applied in each curve's own units.
        const double fraction = -d.y() / (graphRect().height() * m_zoomY);
        TimelineData& data = restoreDragOrigin();
        const QVector<KeyframeRef> moved = ko::moveKeys(data, m_dragKeys, dt);
        if (fraction != 0.0) {
            for (qsizetype i = 0; i < m_dragKeys.size() && i < moved.size(); ++i) {
                const KeyframeRef& from = m_dragKeys[i];
                const ParamId id {from.clipId, from.paramPath};
                const ClipData* origin = editops::clipById(dragOrigin(), from.clipId);
                ClipData* target = editops::clipById(data, from.clipId);
                const AnimatedValue* ov = origin ? ko::findValue(*origin, from.paramPath) : nullptr;
                AnimatedValue* tv = target ? ko::findValue(*target, from.paramPath) : nullptr;
                const qsizetype oi = ov ? ov->indexOf(from.frame) : -1;
                const qsizetype ti = tv ? tv->indexOf(moved[i].frame) : -1;
                if (oi < 0 || ti < 0)
                    continue;
                const Range r = rangeOf(id);
                double value = ov->keys[oi].value + fraction * (r.hi - r.lo);
                if (const CurveInfo* ci = info(id); ci && ci->max > ci->min)
                    value = std::clamp(value, ci->min, ci->max);
                tv->keys[ti].value = value;
            }
        }
        notifyDragChanged();
        m_ctx.selection->setKeyframes(moved);
        return;
    }
    if (m_mode == Mode::MoveHandle) {
        TimelineData& data = restoreDragOrigin();
        const ClipData* c = editops::clipById(data, m_pressId.clipId);
        const AnimatedValue* v = c ? ko::findValue(*c, m_pressId.path) : nullptr;
        const qsizetype i = v ? v->indexOf(m_pressLocal) : -1;
        if (i >= 0) {
            const Keyframe k = v->keys[i];
            const double tipT = frameAtX(pos.x()) - double(c->start);
            const double tipV = yToValue(rangeOf(m_pressId), pos.y() + yOffset());
            const bool out = m_pressSide == ko::HandleSide::Out;
            const double dt = out ? tipT - double(k.frame) : double(k.frame) - tipT;
            const double dv = out ? tipV - k.value : k.value - tipV;
            ko::setHandle(data, {m_pressId.clipId, m_pressId.path, m_pressLocal}, m_pressSide, dt,
                dv, !(event->modifiers() & Qt::AltModifier));
        }
        notifyDragChanged();
        return;
    }
    if (m_mode == Mode::RubberBand) {
        m_band = QRectF(m_pressPos, pos).normalized();
        QVector<KeyframeRef> refs = m_baseSelection;
        for (const CurveInfo& ci : std::as_const(m_curves)) {
            if (m_hidden.contains(ci.id))
                continue;
            const AnimatedValue* v = valueOf(ci.id, nullptr);
            if (!v)
                continue;
            for (const Keyframe& k : v->keys) {
                const KeyframeRef ref {ci.id.clipId, ci.id.path, k.frame};
                if (m_band.contains(keyPosAt(ci.id, k.frame)) && !refs.contains(ref))
                    refs.append(ref);
            }
        }
        setSelection(refs);
        update();
    }
}

void CurveView::mouseReleaseEvent(QMouseEvent*)
{
    if (m_mode == Mode::MovePoints || m_mode == Mode::MoveHandle)
        endDragEdit(true);
    m_mode = Mode::None;
    m_dragKeys.clear();
    m_dragRanges.clear();
    m_band = QRectF();
    update();
}

void CurveView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->position().x() < labelWidth())
        return;
    const Hit hit = hitTest(event->position());
    if (hit.frame && !hit.handle) {
        if (const ClipData* c = clipData(hit.id.clipId))
            jumpToFrame(c->start + *hit.frame);
        return;
    }
    addKeyAt(event->position());
}

void CurveView::addKeyAt(QPointF pos)
{
    ensureActive();
    const ClipData* c = clipData(m_active.clipId);
    if (!c || m_active.path.isEmpty())
        return;
    const int64_t frame = std::clamp<int64_t>(std::llround(frameAtX(pos.x()) - double(c->start)), 0,
        std::max<int64_t>(0, c->duration - 1));
    const AnimatedValue* v = ko::findValue(*c, m_active.path);
    if (!v || v->hasKeyAt(frame))
        return;
    m_ctx.project->beginEdit(tr("Add Keyframe"));
    ClipData* target = editops::clipById(m_ctx.project->timeline()->mutableData(), m_active.clipId);
    AnimatedValue* tv = target ? ko::findValue(*target, m_active.path) : nullptr;
    if (tv)
        tv->setKey(frame, tv->valueAt(frame));
    m_ctx.project->endEdit();
    setSelection({{m_active.clipId, m_active.path, frame}});
}

void CurveView::contextMenuEvent(QContextMenuEvent* event)
{
    const Hit hit = hitTest(event->pos());
    if (hit.frame && !isSelected({hit.id.clipId, hit.id.path, *hit.frame}))
        clickSelect({{hit.id.clipId, hit.id.path, *hit.frame}}, Qt::NoModifier);
    showKeyMenu(event->globalPos());
}

void CurveView::wheelEvent(QWheelEvent* event)
{
    const Qt::KeyboardModifiers zoomMods = Qt::ControlModifier | Qt::ShiftModifier;
    if ((event->modifiers() & zoomMods) == zoomMods) {
        const int delta
            = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
        zoomValue(std::pow(kWheelZoomPerStep, delta / 120.0), event->position().y());
        event->accept();
        return;
    }
    event->ignore(); // the owner (timeline, panel) scrolls / zooms
}

} // namespace kk
