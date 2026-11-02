// SPDX-License-Identifier: GPL-3.0-or-later
// DopeSheetView implementation: rows, painting, selection and time drags.
#include "ui/keyframes/DopeSheetView.h"

#include "core/KeyframeOps.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/keyframes/KeyframeTimeMap.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>
#include <limits>

namespace kk {

namespace ko = keyframeops;

namespace {
constexpr int kTopPadding = 1; ///< logical y of the first row
constexpr int kDiamondHitPx = 6; ///< click tolerance around a diamond centre
constexpr double kParamDiamond = 4.5; ///< diamond radius in parameter rows
constexpr double kSummaryDiamond = 3.5; ///< diamond radius in master / group rows
constexpr int kLabelIndent = 10; ///< label x of group rows (parameters: twice that)
} // namespace

void DopeSheetView::setRows(const QVector<KeyRow>& rows)
{
    m_rows = rows;
    update();
}

void DopeSheetView::setRowHeight(int px)
{
    m_rowHeight = std::max(8, px);
    update();
}

void DopeSheetView::setDrawLabels(bool on)
{
    m_drawLabels = on;
    update();
}

int DopeSheetView::contentHeight() const
{
    return int(m_rows.size()) * m_rowHeight + 2 * kTopPadding;
}

QPoint DopeSheetView::diamondPosAt(int row, int64_t frame) const
{
    const int y = row * m_rowHeight + kTopPadding + m_rowHeight / 2 - yOffset();
    return QPoint(int(std::lround(timeMap().frameToX(double(frame)))), y);
}

int DopeSheetView::rowAtY(int widgetY) const
{
    const int logical = widgetY + yOffset() - kTopPadding;
    return logical < 0 ? -1 : logical / m_rowHeight;
}

void DopeSheetView::rowMarks(const KeyRow& row, QVector<Mark>& out) const
{
    out.clear();
    for (const ParamId& id : row.params) {
        const ClipData* c = clipData(id.clipId);
        const AnimatedValue* v = c ? ko::findValue(*c, id.path) : nullptr;
        if (!v)
            continue;
        for (const Keyframe& k : v->keys)
            out.append({c->start + k.frame, k.frame < 0 || k.frame >= c->duration});
    }
    // Merge per frame: a diamond is faded only if every key there is outside its clip.
    std::sort(
        out.begin(), out.end(), [](const Mark& a, const Mark& b) { return a.frame < b.frame; });
    qsizetype w = 0;
    for (qsizetype r = 0; r < out.size(); ++r) {
        if (w > 0 && out[w - 1].frame == out[r].frame)
            out[w - 1].outside = out[w - 1].outside && out[r].outside;
        else
            out[w++] = out[r];
    }
    out.resize(w);
}

QVector<KeyframeRef> DopeSheetView::rowKeys(const KeyRow& row) const
{
    QVector<KeyframeRef> refs;
    for (const ParamId& id : row.params) {
        const ClipData* c = clipData(id.clipId);
        const AnimatedValue* v = c ? ko::findValue(*c, id.path) : nullptr;
        if (!v)
            continue;
        for (const Keyframe& k : v->keys)
            refs.append({id.clipId, id.path, k.frame});
    }
    return refs;
}

DopeSheetView::Hit DopeSheetView::hitTest(QPoint pos) const
{
    Hit hit;
    hit.row = rowAtY(pos.y());
    if (hit.row < 0 || hit.row >= m_rows.size()) {
        hit.row = -1;
        return hit;
    }
    if (pos.x() < labelWidth())
        return hit;
    double best = kDiamondHitPx + 1;
    rowMarks(m_rows[hit.row], m_marks);
    for (const Mark& m : std::as_const(m_marks)) {
        const double d = std::abs(timeMap().frameToX(double(m.frame)) - pos.x());
        if (d < best) {
            best = d;
            hit.frame = m.frame;
        }
    }
    return hit;
}

// ---- Painting ----

void DopeSheetView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.translate(0, -yOffset());
    paintBackground(p, QRect(0, yOffset(), width(), height()));
    const QPalette& pal = palette();
    const QColor text = pal.color(QPalette::WindowText);
    const QColor dim = pal.color(QPalette::PlaceholderText);
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() - 1.0));
    QFont bold = small;
    bold.setBold(true);

    const int lw = labelWidth();
    const double left = lw - kParamDiamond;
    const double right = width() + kParamDiamond;
    // Only the rows inside the visible window are visited.
    const int first = std::max(0, rowAtY(0));
    const int last = std::min(int(m_rows.size()) - 1, rowAtY(height()));
    for (int i = first; i <= last; ++i) {
        const KeyRow& row = m_rows[i];
        const QRect r(0, i * m_rowHeight + kTopPadding, width(), m_rowHeight);
        if (row.kind == KeyRow::Kind::Item)
            p.fillRect(QRect(lw, r.top(), width() - lw, r.height()), withAlpha(text, 22));
        else if (i % 2 == 1)
            p.fillRect(QRect(lw, r.top(), width() - lw, r.height()), withAlpha(text, 9));
        if (lw > 0 && m_drawLabels) {
            const int indent = row.kind == KeyRow::Kind::Param ? 2 * kLabelIndent
                : row.kind == KeyRow::Kind::Group              ? kLabelIndent
                                                               : 6;
            p.setFont(row.kind == KeyRow::Kind::Param ? small : bold);
            p.setPen(row.kind == KeyRow::Kind::Group ? dim : text);
            p.drawText(QRect(indent, r.top(), lw - indent - 6, r.height()),
                Qt::AlignVCenter | Qt::AlignLeft,
                p.fontMetrics().elidedText(row.label, Qt::ElideRight, lw - indent - 6));
        }

        const double cy = r.center().y() + 0.5;
        rowMarks(row, m_marks);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Parameter rows: a thin line between the first and last key shows the animated span.
        if (row.kind == KeyRow::Kind::Param && m_marks.size() > 1) {
            const double x0 = std::max(left, timeMap().frameToX(double(m_marks.front().frame)));
            const double x1 = std::min(right, timeMap().frameToX(double(m_marks.back().frame)));
            if (x1 > x0) {
                // Text colour, faint: readable on dark and light lanes alike.
                QColor span = text;
                span.setAlpha(70);
                p.setPen(QPen(span, 1));
                p.drawLine(QPointF(x0, cy), QPointF(x1, cy));
            }
        }
        const double radius = row.kind == KeyRow::Kind::Param ? kParamDiamond : kSummaryDiamond;
        for (const Mark& m : std::as_const(m_marks)) {
            const double x = timeMap().frameToX(double(m.frame));
            if (x < left || x > right)
                continue;
            bool selected = false;
            for (const KeyframeRef& ref : keysAtFrame(m.frame, row.params)) {
                if (isSelected(ref)) {
                    selected = true;
                    break;
                }
            }
            // Keys outside the clip (trimmed away) are kept but drawn faded.
            // Same colours as the keyframe marks on the clip body and the timeline selection.
            QColor fill = selected ? schemeColors().keyframeSelected : schemeColors().keyframe;
            if (row.kind != KeyRow::Kind::Param && !selected)
                fill = fill.darker(125);
            if (m.outside)
                fill.setAlpha(90);
            p.setPen(QPen(withAlpha(schemeColors().shadow, m.outside ? 90 : 255), 1));
            p.setBrush(fill);
            drawDiamond(p, QPointF(x, cy), radius);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(Qt::NoBrush);
    }
    paintPlayhead(p);
    p.translate(0, yOffset());
    paintBand(p, m_band);
}

// ---- Mouse ----

void DopeSheetView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    const Hit hit = hitTest(pos);
    m_pressPos = pos;
    if (hit.row >= 0 && hit.frame) {
        clickSelect(keysAtFrame(*hit.frame, m_rows[hit.row].params), event->modifiers());
        m_pressFrame = *hit.frame;
        m_mode = Mode::Pending;
        return;
    }
    if (hit.row >= 0 && pos.x() < labelWidth() && m_drawLabels) {
        clickSelect(rowKeys(m_rows[hit.row]), event->modifiers()); // label: every key of the row
        return;
    }
    // Empty space: rubber band (Shift / Ctrl keep the current selection).
    m_baseSelection = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)
        ? selectedKeys()
        : QVector<KeyframeRef> {};
    setSelection(m_baseSelection);
    m_mode = Mode::RubberBand;
    m_band = QRect(pos, QSize(1, 1));
    update();
}

void DopeSheetView::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    if (m_mode == Mode::Pending
        && (pos - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_dragKeys = selectedKeys();
        if (m_dragKeys.isEmpty()) {
            m_mode = Mode::None;
            return;
        }
        beginDragEdit(tr("Move Keyframes"));
        m_mode = Mode::Move;
    }
    if (m_mode == Mode::Move && !dragEditOpen()) {
        m_mode = Mode::None; // cancelled with Esc
        return;
    }
    if (m_mode == Mode::Move) {
        const int64_t raw = std::llround(frameAtX(pos.x()) - frameAtX(m_pressPos.x()));
        int64_t delta = snapFrame(m_pressFrame + raw) - m_pressFrame;
        delta = ko::clampMoveDelta(dragOrigin(), m_dragKeys, delta);
        TimelineData& data = restoreDragOrigin();
        const QVector<KeyframeRef> moved = ko::moveKeys(data, m_dragKeys, delta);
        notifyDragChanged();
        m_ctx.selection->setKeyframes(moved);
        return;
    }
    if (m_mode == Mode::RubberBand) {
        m_band = QRect(m_pressPos, pos).normalized();
        const int r0 = std::max(0, rowAtY(m_band.top()));
        const int r1 = std::min(int(m_rows.size()) - 1, rowAtY(m_band.bottom()));
        const int64_t f0 = int64_t(std::ceil(frameAtX(m_band.left())));
        const int64_t f1 = int64_t(std::floor(frameAtX(m_band.right())));
        QVector<KeyframeRef> refs = m_baseSelection;
        for (int r = r0; r <= r1; ++r) {
            for (const ParamId& id : m_rows[r].params) {
                const ClipData* c = clipData(id.clipId);
                if (!c)
                    continue;
                for (const KeyframeRef& ref :
                    ko::keysInRange(*c, f0 - c->start, f1 - c->start, {id.path})) {
                    if (!refs.contains(ref))
                        refs.append(ref);
                }
            }
        }
        setSelection(refs);
        update();
    }
}

void DopeSheetView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_mode == Mode::Move)
        endDragEdit(true);
    m_mode = Mode::None;
    m_dragKeys.clear();
    m_band = QRect();
    update();
}

void DopeSheetView::mouseDoubleClickEvent(QMouseEvent* event)
{
    const Hit hit = hitTest(event->position().toPoint());
    if (hit.row >= 0 && hit.frame)
        jumpToFrame(*hit.frame);
}

void DopeSheetView::contextMenuEvent(QContextMenuEvent* event)
{
    const Hit hit = hitTest(event->pos());
    if (hit.row >= 0 && hit.frame) {
        const QVector<KeyframeRef> refs = keysAtFrame(*hit.frame, m_rows[hit.row].params);
        const bool anySelected = std::any_of(
            refs.cbegin(), refs.cend(), [this](const KeyframeRef& r) { return isSelected(r); });
        if (!anySelected)
            clickSelect(refs, Qt::NoModifier);
    }
    showKeyMenu(event->globalPos());
}

} // namespace kk
