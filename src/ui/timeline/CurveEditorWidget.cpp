// SPDX-License-Identifier: GPL-3.0-or-later
// CurveEditorWidget implementation: value mapping, curve drawing, key and handle drags.
#include "ui/timeline/CurveEditorWidget.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelineLayout.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QWheelEvent>

#include <cmath>

namespace kk {

using namespace timelinemetrics;
namespace ko = keyframeops;

namespace {
constexpr int kListRowHeight = 16; ///< parameter list rows in the header column
constexpr int kListTop = 4;
constexpr int kButtonSize = 22; ///< tool bar buttons (16 px icons)
constexpr int kGraphMargin = 10; ///< vertical padding of the graph area
constexpr double kPointRadius = 3.5;
constexpr double kHitPx = 6.0;
constexpr double kSampleStepPx = 2.0; ///< curve polyline resolution
constexpr double kWheelZoomPerStep = 1.15; ///< value zoom per wheel notch (120 units)
const QColor kKeySelected(0xff, 0xa8, 0x3a);

// Own curve palette: distinct mid-saturation hues readable on dark and light lanes.
const QColor kCurveColors[] = {QColor(0xe0, 0x5c, 0x5c), QColor(0x5c, 0xc0, 0x6c),
    QColor(0x5c, 0x9c, 0xe8), QColor(0xe0, 0xc0, 0x48), QColor(0xc0, 0x6c, 0xe0),
    QColor(0x48, 0xc8, 0xc8), QColor(0xe8, 0x8c, 0x40), QColor(0xa8, 0xa8, 0xa8)};

// "Nice" grid step (1, 2, 5 x 10^n) giving about @p lines lines over @p span.
double niceStep(double span, int lines)
{
    const double raw = span / std::max(1, lines);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        if (m * mag >= raw)
            return m * mag;
    }
    return 10.0 * mag;
}
} // namespace

CurveEditorWidget::CurveEditorWidget(AppContext& ctx, const TimelineLayout& layout,
    TimelineController* controller, KeyframeLanes* lanes, quint64 clipId, QWidget* parent)
    : ClipLaneWidget(ctx, layout, controller, lanes, clipId, parent)
{
    struct Tool
    {
        const char* icon;
        const char* tip;
        std::optional<Interp> interp;
        int special; ///< 1 = flatten, 2 = fit
    };
    const Tool tools[] = {
        {"interp_linear", QT_TR_NOOP("Linear"), Interp::Linear, 0},
        {"interp_smooth", QT_TR_NOOP("Smooth (Bezier)"), Interp::Smooth, 0},
        {"interp_ease_in", QT_TR_NOOP("Ease In"), Interp::EaseIn, 0},
        {"interp_ease_out", QT_TR_NOOP("Ease Out"), Interp::EaseOut, 0},
        {"interp_ease_in_out", QT_TR_NOOP("Ease In and Out"), Interp::EaseInOut, 0},
        {"interp_hold", QT_TR_NOOP("Static (Hold)"), Interp::Hold, 0},
        {"interp_flatten", QT_TR_NOOP("Flatten Handles"), std::nullopt, 1},
        {"curve_fit", QT_TR_NOOP("Fit Curves"), std::nullopt, 2},
    };
    for (const Tool& t : tools) {
        auto* b = new QToolButton(this);
        b->setAutoRaise(true);
        b->setIcon(kk::icon(QLatin1String(t.icon)));
        b->setIconSize(QSize(metrics::kToolIconSize, metrics::kToolIconSize));
        b->setFixedSize(kButtonSize, kButtonSize);
        b->setToolTip(tr(t.tip));
        b->setObjectName(QLatin1String(t.icon));
        b->setFocusPolicy(Qt::NoFocus);
        const std::optional<Interp> interp = t.interp;
        const int special = t.special;
        connect(b, &QToolButton::clicked, this, [this, interp, special] {
            if (interp)
                m_lanes->setSelectedInterp(*interp);
            else if (special == 1)
                m_lanes->flattenSelected();
            else
                fitView();
        });
        m_buttons.append(b);
    }
    m_polyline.reserve(4096);
}

QVector<ko::AnimatedParam> CurveEditorWidget::params() const
{
    const ClipData* c = clip();
    return c ? ko::animatedParams(*c) : QVector<ko::AnimatedParam> {};
}

QVector<QString> CurveEditorWidget::shownPaths() const
{
    QVector<QString> out;
    for (const ko::AnimatedParam& p : params()) {
        if (!m_hidden.contains(p.path))
            out.append(p.path);
    }
    return out;
}

void CurveEditorWidget::setCurveShown(const QString& path, bool shown)
{
    if (shown) {
        m_hidden.remove(path);
        m_active = path;
    } else {
        m_hidden.insert(path);
        if (m_active == path)
            m_active.clear();
    }
    update();
}

void CurveEditorWidget::fitView()
{
    m_zoomY = 1.0;
    m_panY = 0.0;
    update();
}

QColor CurveEditorWidget::curveColor(int index) const
{
    return kCurveColors[size_t(index) % std::size(kCurveColors)];
}

// ---- Geometry ----

QRectF CurveEditorWidget::graphRect() const
{
    return QRectF(kHeaderWidth, kGraphMargin, std::max(1, width() - kHeaderWidth),
        std::max(10, laneHeight() - 2 * kGraphMargin));
}

CurveEditorWidget::Range CurveEditorWidget::rangeOf(const QString& path) const
{
    const ClipData* c = clip();
    const AnimatedValue* v = c ? ko::findValue(*c, path) : nullptr;
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

double CurveEditorWidget::valueToY(const Range& r, double value) const
{
    const QRectF g = graphRect();
    const double n = (value - r.lo) / (r.hi - r.lo);
    const double z = (n - 0.5 - m_panY) * m_zoomY + 0.5;
    return g.bottom() - z * g.height();
}

double CurveEditorWidget::yToValue(const Range& r, double logicalY) const
{
    const QRectF g = graphRect();
    const double z = (g.bottom() - logicalY) / g.height();
    const double n = (z - 0.5) / m_zoomY + 0.5 + m_panY;
    return r.lo + n * (r.hi - r.lo);
}

bool CurveEditorWidget::handleVisible(const AnimatedValue& v, qsizetype i, bool out) const
{
    // Handles exist where a segment does and are only shown for curved sides (a Linear side
    // has no handle in Resolve-style editors; pick Smooth or an ease to get one).
    if (out && (i + 1 >= v.keys.size() || v.keys[i].hold))
        return false;
    if (!out && (i == 0 || v.keys[i - 1].hold))
        return false;
    return (out ? v.keys[i].out.mode : v.keys[i].in.mode) != KeyHandle::Mode::Linear;
}

QPointF CurveEditorWidget::keyPos(const QString& path, int64_t localFrame) const
{
    const ClipData* c = clip();
    const AnimatedValue* v = c ? ko::findValue(*c, path) : nullptr;
    const qsizetype i = v ? v->indexOf(localFrame) : -1;
    if (i < 0)
        return {};
    return QPointF(
        xOfLocal(double(localFrame)), valueToY(rangeOf(path), v->keys[i].value) - yOffset());
}

QPointF CurveEditorWidget::handlePos(
    const QString& path, int64_t localFrame, ko::HandleSide side) const
{
    const ClipData* c = clip();
    const AnimatedValue* v = c ? ko::findValue(*c, path) : nullptr;
    const qsizetype i = v ? v->indexOf(localFrame) : -1;
    if (i < 0)
        return {};
    const Keyframe& k = v->keys[i];
    const Range r = rangeOf(path);
    if (side == ko::HandleSide::Out) {
        const auto [dt, dv] = v->outHandle(i);
        return QPointF(xOfLocal(double(k.frame) + dt), valueToY(r, k.value + dv) - yOffset());
    }
    const auto [dt, dv] = v->inHandle(i);
    return QPointF(xOfLocal(double(k.frame) - dt), valueToY(r, k.value - dv) - yOffset());
}

CurveEditorWidget::Hit CurveEditorWidget::hitTest(QPointF pos) const
{
    Hit hit;
    const double logicalY = pos.y() + yOffset();
    const QVector<ko::AnimatedParam> all = params();
    if (pos.x() < kHeaderWidth) {
        const int row = int(std::floor((logicalY - kListTop) / kListRowHeight));
        if (row >= 0 && row < all.size() && logicalY < laneHeight() - kButtonSize - 6)
            hit.listRow = row;
        return hit;
    }
    const ClipData* c = clip();
    if (!c)
        return hit;
    double best = kHitPx;
    // Handle tips of selected keys first: they sit on top of the curve.
    for (const KeyframeRef& ref : selectedKeys()) {
        if (m_hidden.contains(ref.paramPath))
            continue;
        const AnimatedValue* v = ko::findValue(*c, ref.paramPath);
        const qsizetype i = v ? v->indexOf(ref.frame) : -1;
        if (i < 0)
            continue;
        for (const bool out : {false, true}) {
            if (!handleVisible(*v, i, out))
                continue;
            const QPointF tip = handlePos(
                ref.paramPath, ref.frame, out ? ko::HandleSide::Out : ko::HandleSide::In);
            const double d = QLineF(tip, pos).length();
            if (d < best) {
                best = d;
                hit.path = ref.paramPath;
                hit.frame = ref.frame;
                hit.handle = out ? ko::HandleSide::Out : ko::HandleSide::In;
            }
        }
    }
    if (hit.handle)
        return hit;
    for (const ko::AnimatedParam& p : all) {
        if (m_hidden.contains(p.path))
            continue;
        const AnimatedValue* v = ko::findValue(*c, p.path);
        if (!v)
            continue;
        const Range r = rangeOf(p.path);
        for (const Keyframe& k : v->keys) {
            const QPointF kp(xOfLocal(double(k.frame)), valueToY(r, k.value) - yOffset());
            const double d = QLineF(kp, pos).length();
            // Ties prefer the active curve.
            if (d < best || (d <= best && p.path == m_active)) {
                best = d;
                hit.path = p.path;
                hit.frame = k.frame;
            }
        }
    }
    return hit;
}

void CurveEditorWidget::resizeEvent(QResizeEvent* event)
{
    ClipLaneWidget::resizeEvent(event);
    layoutButtons();
}

void CurveEditorWidget::layoutButtons()
{
    // Tool bar along the bottom of the header column (logical y, shifted by the visible cut).
    const int y = laneHeight() - kButtonSize - 4 - yOffset();
    int x = metrics::kSpaceS;
    for (QToolButton* b : m_buttons) {
        b->move(x, y);
        b->setVisible(y >= 0);
        x += kButtonSize + 1;
    }
}

// ---- Painting ----

void CurveEditorWidget::paintEvent(QPaintEvent*)
{
    layoutButtons(); // the visible cut may have changed without a resize
    QPainter p(this);
    p.translate(0, -yOffset());
    paintLaneBackground(p, QRect(0, 0, width(), laneHeight()));
    const ClipData* c = clip();
    if (!c)
        return;
    const QPalette& pal = palette();
    const QColor text = pal.color(QPalette::WindowText);
    const QColor dim = pal.color(QPalette::PlaceholderText);
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() - 1.0));
    p.setFont(small);
    const QVector<ko::AnimatedParam> all = params();
    if (m_active.isEmpty() || m_hidden.contains(m_active)) {
        for (const ko::AnimatedParam& prm : all) {
            if (!m_hidden.contains(prm.path)) {
                const_cast<CurveEditorWidget*>(this)->m_active = prm.path;
                break;
            }
        }
    }

    // Parameter list.
    const int listBottom = laneHeight() - kButtonSize - 6;
    for (int i = 0; i < all.size(); ++i) {
        const int y = kListTop + i * kListRowHeight;
        if (y + kListRowHeight > listBottom)
            break;
        const bool shown = !m_hidden.contains(all[i].path);
        const QColor col = curveColor(i);
        const QRect box(8, y + 4, 9, 9);
        p.setPen(col);
        p.setBrush(shown ? col : Qt::NoBrush);
        p.drawRect(box);
        p.setBrush(Qt::NoBrush);
        QFont f = small;
        f.setBold(all[i].path == m_active);
        p.setFont(f);
        p.setPen(shown ? text : dim);
        const QString label = all[i].groupName + QStringLiteral(" / ") + all[i].label;
        p.drawText(QRect(22, y, kHeaderWidth - 28, kListRowHeight), Qt::AlignVCenter,
            p.fontMetrics().elidedText(label, Qt::ElideRight, kHeaderWidth - 28));
    }
    p.setFont(small);

    const QRectF g = graphRect();
    p.save();
    p.setClipRect(QRectF(kHeaderWidth, 0, width() - kHeaderWidth, laneHeight()));
    // Value grid and axis labels of the active curve.
    if (!m_active.isEmpty()) {
        const Range r = rangeOf(m_active);
        const double vTop = yToValue(r, g.top());
        const double vBottom = yToValue(r, g.bottom());
        const double step = niceStep(std::abs(vTop - vBottom), 4);
        QColor grid = text;
        grid.setAlpha(28);
        for (double v = std::ceil(std::min(vTop, vBottom) / step) * step;
             v <= std::max(vTop, vBottom); v += step) {
            const double y = std::round(valueToY(r, v)) + 0.5;
            p.setPen(grid);
            p.drawLine(QPointF(g.left(), y), QPointF(g.right(), y));
            p.setPen(dim);
            p.drawText(QPointF(g.left() + 4, y - 2), QString::number(v, 'g', 5));
        }
    }
    // Clip edges.
    p.setPen(QPen(dim, 1, Qt::DotLine));
    for (const double f : {0.0, double(c->duration)}) {
        const double x = std::round(xOfLocal(f)) + 0.5;
        p.drawLine(QPointF(x, 0), QPointF(x, laneHeight()));
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    const double xMin = g.left();
    const double xMax = g.right();
    for (int i = 0; i < all.size(); ++i) {
        const QString& path = all[i].path;
        if (m_hidden.contains(path))
            continue;
        const AnimatedValue* v = ko::findValue(*c, path);
        if (!v || v->keys.isEmpty())
            continue;
        const Range r = rangeOf(path);
        const QColor col = curveColor(i);
        // Curve: sampled every kSampleStepPx across the view (allocation-free after warm-up).
        m_polyline.clear();
        // Only across the clip (and keys left outside it by a trim), like the clip itself.
        const double c0
            = std::max(xMin, xOfLocal(double(std::min<int64_t>(0, v->keys.front().frame))));
        const double c1 = std::min(
            xMax, xOfLocal(double(std::max<int64_t>(c->duration, v->keys.back().frame))));
        for (double x = c0; x <= c1 + kSampleStepPx; x += kSampleStepPx)
            m_polyline.append(
                QPointF(std::min(x, c1), valueToY(r, v->valueAtTime(localAtX(std::min(x, c1))))));
        QColor line = col;
        if (path != m_active)
            line.setAlpha(170);
        p.setPen(QPen(line, path == m_active ? 1.6 : 1.2));
        p.drawPolyline(m_polyline);

        // Keys and the handles of selected keys.
        for (qsizetype k = 0; k < v->keys.size(); ++k) {
            const Keyframe& key = v->keys[k];
            const QPointF kp(xOfLocal(double(key.frame)), valueToY(r, key.value));
            if (kp.x() < xMin - 10 || kp.x() > xMax + 10)
                continue;
            const bool selected = isSelected({clipId(), path, key.frame});
            if (selected) {
                for (const bool out : {false, true}) {
                    if (!handleVisible(*v, k, out))
                        continue;
                    const auto [dt, dv] = out ? v->outHandle(k) : v->inHandle(k);
                    const QPointF tip(xOfLocal(double(key.frame) + (out ? dt : -dt)),
                        valueToY(r, key.value + (out ? dv : -dv)));
                    p.setPen(QPen(text, 1));
                    p.drawLine(kp, tip);
                    p.setBrush(pal.color(QPalette::Base));
                    p.drawEllipse(tip, 3.0, 3.0);
                }
            }
            p.setPen(QPen(QColor(20, 20, 20), 1));
            p.setBrush(selected ? kKeySelected : col);
            if (key.hold)
                p.drawRect(QRectF(kp.x() - kPointRadius, kp.y() - kPointRadius, 2 * kPointRadius,
                    2 * kPointRadius));
            else
                drawDiamond(p, kp, kPointRadius + 0.5);
            p.setBrush(Qt::NoBrush);
        }
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    p.restore();
    paintPlayhead(p);
    if (!m_band.isEmpty()) {
        p.translate(0, yOffset());
        QColor fill = pal.color(QPalette::Highlight);
        fill.setAlpha(50);
        p.fillRect(m_band, fill);
        p.setPen(pal.color(QPalette::Highlight));
        p.drawRect(m_band.adjusted(0, 0, -1, -1));
    }
}

// ---- Mouse ----

void CurveEditorWidget::mousePressEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    m_pressPos = pos;
    m_pressModifiers = event->modifiers();
    m_axisLock = 0;
    if (event->button() == Qt::MiddleButton) {
        m_mode = Mode::PanValue;
        m_panAtPress = m_panY;
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const Hit hit = hitTest(pos);
    if (hit.listRow >= 0) {
        const QVector<ko::AnimatedParam> all = params();
        const QString& path = all[hit.listRow].path;
        setCurveShown(path, m_hidden.contains(path));
        return;
    }
    if (hit.frame) {
        m_pressPath = hit.path;
        m_pressFrame = *hit.frame;
        m_active = hit.path;
        if (hit.handle) {
            m_pressSide = *hit.handle;
            m_mode = Mode::MoveHandle;
            m_dragRanges.clear();
            m_dragRanges.insert(hit.path, rangeOf(hit.path));
            beginDragEdit(tr("Change Keyframe Handle"));
            return;
        }
        clickSelect({{clipId(), hit.path, *hit.frame}}, event->modifiers());
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

void CurveEditorWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    if (m_mode == Mode::PanValue) {
        m_panY = m_panAtPress + (pos.y() - m_pressPos.y()) / (graphRect().height() * m_zoomY);
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
        for (const KeyframeRef& r : m_dragKeys) {
            if (!m_dragRanges.contains(r.paramPath))
                m_dragRanges.insert(r.paramPath, rangeOf(r.paramPath));
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
        int64_t dt = 0;
        if (d.x() != 0.0) {
            const int64_t raw
                = std::llround(localAtX(m_pressPos.x() + d.x()) - localAtX(m_pressPos.x()));
            dt = snapLocal(m_pressFrame + raw) - m_pressFrame;
            dt = ko::clampMoveDelta(dragOrigin(), m_dragKeys, dt);
        }
        // Value change as a fraction of the graph, applied in each curve's own units.
        const double fraction = -d.y() / (graphRect().height() * m_zoomY);
        TimelineData& data = restoreDragOrigin();
        const QVector<KeyframeRef> moved = ko::moveKeys(data, m_dragKeys, dt);
        const ClipData* origin = editops::clipById(dragOrigin(), clipId());
        ClipData* target = editops::clipById(data, clipId());
        if (origin && target && fraction != 0.0) {
            const QVector<ko::AnimatedParam> all = ko::animatedParams(*origin);
            for (qsizetype i = 0; i < m_dragKeys.size() && i < moved.size(); ++i) {
                const QString& path = m_dragKeys[i].paramPath;
                const AnimatedValue* ov = ko::findValue(*origin, path);
                AnimatedValue* tv = ko::findValue(*target, path);
                const qsizetype oi = ov ? ov->indexOf(m_dragKeys[i].frame) : -1;
                const qsizetype ti = tv ? tv->indexOf(moved[i].frame) : -1;
                if (oi < 0 || ti < 0)
                    continue;
                const Range r = m_dragRanges.value(path);
                double value = ov->keys[oi].value + fraction * (r.hi - r.lo);
                for (const ko::AnimatedParam& prm : all) {
                    if (prm.path == path && prm.max > prm.min)
                        value = std::clamp(value, prm.min, prm.max);
                }
                tv->keys[ti].value = value;
            }
        }
        notifyDragChanged();
        m_ctx.selection->setKeyframes(moved);
        return;
    }
    if (m_mode == Mode::MoveHandle) {
        TimelineData& data = restoreDragOrigin();
        const ClipData* c = editops::clipById(data, clipId());
        const AnimatedValue* v = c ? ko::findValue(*c, m_pressPath) : nullptr;
        const qsizetype i = v ? v->indexOf(m_pressFrame) : -1;
        if (i >= 0) {
            const Keyframe k = v->keys[i];
            const double tipT = localAtX(pos.x());
            const double tipV = yToValue(m_dragRanges.value(m_pressPath), pos.y() + yOffset());
            const bool out = m_pressSide == ko::HandleSide::Out;
            const double dt = out ? tipT - double(k.frame) : double(k.frame) - tipT;
            const double dv = out ? tipV - k.value : k.value - tipV;
            ko::setHandle(data, {clipId(), m_pressPath, m_pressFrame}, m_pressSide, dt, dv,
                !(event->modifiers() & Qt::AltModifier));
        }
        notifyDragChanged();
        return;
    }
    if (m_mode == Mode::RubberBand) {
        m_band = QRectF(m_pressPos, pos).normalized();
        const ClipData* c = clip();
        if (!c)
            return;
        QVector<KeyframeRef> refs = m_baseSelection;
        for (const ko::AnimatedParam& prm : params()) {
            if (m_hidden.contains(prm.path))
                continue;
            const AnimatedValue* v = ko::findValue(*c, prm.path);
            for (const Keyframe& k : v->keys) {
                const KeyframeRef ref {clipId(), prm.path, k.frame};
                if (m_band.contains(keyPos(prm.path, k.frame)) && !refs.contains(ref))
                    refs.append(ref);
            }
        }
        setSelection(refs);
        update();
    }
}

void CurveEditorWidget::mouseReleaseEvent(QMouseEvent*)
{
    if (m_mode == Mode::MovePoints || m_mode == Mode::MoveHandle)
        endDragEdit(true);
    m_mode = Mode::None;
    m_dragKeys.clear();
    m_band = QRectF();
    update();
}

void CurveEditorWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    const Hit hit = hitTest(event->position());
    if (hit.frame && !hit.handle) {
        jumpTo(*hit.frame);
        return;
    }
    if (hit.listRow < 0 && event->position().x() >= kHeaderWidth)
        addKeyAt(event->position());
}

void CurveEditorWidget::addKeyAt(QPointF pos)
{
    const ClipData* c = clip();
    if (!c || m_active.isEmpty())
        return;
    const int64_t frame = std::clamp<int64_t>(
        std::llround(localAtX(pos.x())), 0, std::max<int64_t>(0, c->duration - 1));
    const AnimatedValue* v = ko::findValue(*c, m_active);
    if (!v || v->hasKeyAt(frame))
        return;
    m_ctx.project->beginEdit(tr("Add Keyframe"));
    ClipData* target = editops::clipById(m_ctx.project->timeline()->mutableData(), clipId());
    AnimatedValue* tv = target ? ko::findValue(*target, m_active) : nullptr;
    if (tv)
        tv->setKey(frame, tv->valueAt(frame));
    m_ctx.project->endEdit();
    setSelection({{clipId(), m_active, frame}});
}

void CurveEditorWidget::contextMenuEvent(QContextMenuEvent* event)
{
    const Hit hit = hitTest(event->pos());
    if (hit.frame && !isSelected({clipId(), hit.path, *hit.frame}))
        clickSelect({{clipId(), hit.path, *hit.frame}}, Qt::NoModifier);
    showKeyMenu(event->globalPos());
}

void CurveEditorWidget::wheelEvent(QWheelEvent* event)
{
    const Qt::KeyboardModifiers zoomMods = Qt::ControlModifier | Qt::ShiftModifier;
    if ((event->modifiers() & zoomMods) == zoomMods) {
        const int delta
            = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
        m_zoomY = std::clamp(m_zoomY * std::pow(kWheelZoomPerStep, delta / 120.0), 0.05, 200.0);
        update();
        event->accept();
        return;
    }
    event->ignore(); // the timeline scrolls / zooms as usual
}

} // namespace kk
