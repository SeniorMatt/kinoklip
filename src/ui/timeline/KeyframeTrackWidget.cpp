// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeTrackWidget implementation: rows, painting, selection and time drags.
#include "ui/timeline/KeyframeTrackWidget.h"

#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineLayout.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>
#include <limits>

namespace kk {

using namespace timelinemetrics;
namespace ko = keyframeops;

namespace {
constexpr int kDiamondHitPx = 6; ///< click tolerance around a diamond centre
constexpr double kParamDiamond = 4.5; ///< diamond radius in parameter rows
constexpr double kSummaryDiamond = 3.5; ///< diamond radius in master / group rows
constexpr int kLabelIndent = 10; ///< label x of group rows (parameters: twice that)
const QColor kKeyFill(0xf4, 0xe2, 0x9a); ///< same as the marks on the clip body
const QColor kKeySelected(0xff, 0xa8, 0x3a); ///< timeline selection colour
} // namespace

int KeyframeTrackWidget::heightFor(const ClipData& clip)
{
    // Master + groups + parameters.
    const QVector<ko::AnimatedParam> params = ko::animatedParams(clip);
    int groups = 0;
    QString last;
    for (const ko::AnimatedParam& p : params) {
        if (p.groupId != last) {
            ++groups;
            last = p.groupId;
        }
    }
    return (1 + groups + int(params.size())) * kRowHeight + 2;
}

QVector<KeyframeTrackWidget::Row> KeyframeTrackWidget::rows() const
{
    QVector<Row> out;
    const ClipData* c = clip();
    if (!c)
        return out;
    const QVector<ko::AnimatedParam> params = ko::animatedParams(*c);
    Row master {Row::Kind::Master, tr("Keyframes"), {}};
    for (const ko::AnimatedParam& p : params)
        master.paths.append(p.path);
    out.append(master);
    QString last;
    for (const ko::AnimatedParam& p : params) {
        if (p.groupId != last) {
            last = p.groupId;
            Row g {Row::Kind::Group, p.groupName, {}};
            for (const ko::AnimatedParam& q : params) {
                if (q.groupId == p.groupId)
                    g.paths.append(q.path);
            }
            out.append(g);
        }
        out.append(Row {Row::Kind::Param, p.label, {p.path}});
    }
    return out;
}

QPoint KeyframeTrackWidget::diamondPos(int row, int64_t localFrame) const
{
    const int y = row * kRowHeight + 1 + kRowHeight / 2 - yOffset();
    return QPoint(int(std::lround(xOfLocal(double(localFrame)))), y);
}

int KeyframeTrackWidget::rowAtY(int widgetY) const
{
    const int logical = widgetY + yOffset() - 1;
    return logical < 0 ? -1 : logical / kRowHeight;
}

QVector<int64_t> KeyframeTrackWidget::rowFrames(const Row& row) const
{
    QVector<int64_t> frames;
    const ClipData* c = clip();
    if (!c)
        return frames;
    for (const QString& path : row.paths) {
        if (const AnimatedValue* v = ko::findValue(*c, path)) {
            for (const Keyframe& k : v->keys)
                frames.append(k.frame);
        }
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

QVector<KeyframeRef> KeyframeTrackWidget::refsAt(const Row& row, int64_t frame) const
{
    QVector<KeyframeRef> refs;
    const ClipData* c = clip();
    if (!c)
        return refs;
    for (const QString& path : row.paths) {
        const AnimatedValue* v = ko::findValue(*c, path);
        if (v && v->hasKeyAt(frame))
            refs.append({clipId(), path, frame});
    }
    return refs;
}

KeyframeTrackWidget::Hit KeyframeTrackWidget::hitTest(QPoint pos) const
{
    Hit hit;
    const QVector<Row> all = rows();
    hit.row = rowAtY(pos.y());
    if (hit.row < 0 || hit.row >= all.size()) {
        hit.row = -1;
        return hit;
    }
    if (pos.x() < kHeaderWidth)
        return hit;
    double best = kDiamondHitPx + 1;
    for (int64_t f : rowFrames(all[hit.row])) {
        const double d = std::abs(xOfLocal(double(f)) - pos.x());
        if (d < best) {
            best = d;
            hit.frame = f;
        }
    }
    return hit;
}

// ---- Painting ----

void KeyframeTrackWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.translate(0, -yOffset());
    const QRect lane(0, 0, width(), laneHeight());
    paintLaneBackground(p, lane);
    const ClipData* c = clip();
    if (!c)
        return;
    const QPalette& pal = palette();
    const QVector<Row> all = rows();
    const QColor text = pal.color(QPalette::WindowText);
    const QColor dim = pal.color(QPalette::PlaceholderText);
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() - 1.0));
    QFont bold = small;
    bold.setBold(true);

    const double left = kHeaderWidth - kParamDiamond;
    const double right = width() + kParamDiamond;
    for (int i = 0; i < all.size(); ++i) {
        const Row& row = all[i];
        const QRect r(0, i * kRowHeight + 1, width(), kRowHeight);
        if (i % 2 == 1)
            p.fillRect(QRect(kHeaderWidth, r.top(), width() - kHeaderWidth, r.height()),
                QColor(127, 127, 127, 14));
        // Label in the header column.
        const int indent = row.kind == Row::Kind::Master ? 6
            : row.kind == Row::Kind::Group               ? kLabelIndent
                                                         : 2 * kLabelIndent;
        p.setFont(row.kind == Row::Kind::Param ? small : bold);
        p.setPen(
            row.kind == Row::Kind::Param ? text : (row.kind == Row::Kind::Master ? text : dim));
        p.drawText(QRect(indent, r.top(), kHeaderWidth - indent - 6, r.height()),
            Qt::AlignVCenter | Qt::AlignLeft,
            p.fontMetrics().elidedText(row.label, Qt::ElideRight, kHeaderWidth - indent - 6));

        const double cy = r.center().y() + 0.5;
        const QVector<int64_t> frames = rowFrames(row);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Parameter rows: a thin line between the first and last key shows the animated span.
        if (row.kind == Row::Kind::Param && frames.size() > 1) {
            const double x0 = std::max(left, xOfLocal(double(frames.front())));
            const double x1 = std::min(right, xOfLocal(double(frames.back())));
            if (x1 > x0) {
                // Text colour, faint: readable on dark and light lanes alike.
                QColor span = text;
                span.setAlpha(70);
                p.setPen(QPen(span, 1));
                p.drawLine(QPointF(x0, cy), QPointF(x1, cy));
            }
        }
        const double radius = row.kind == Row::Kind::Param ? kParamDiamond : kSummaryDiamond;
        for (int64_t f : frames) {
            const double x = xOfLocal(double(f));
            if (x < left || x > right)
                continue;
            bool selected = false;
            for (const KeyframeRef& ref : refsAt(row, f)) {
                if (isSelected(ref)) {
                    selected = true;
                    break;
                }
            }
            // Keys outside the clip (trimmed away) are kept but drawn faded.
            const bool outside = f < 0 || f >= c->duration;
            QColor fill = selected ? kKeySelected : kKeyFill;
            if (row.kind != Row::Kind::Param && !selected)
                fill = fill.darker(125);
            if (outside)
                fill.setAlpha(90);
            p.setPen(QPen(QColor(20, 20, 20, outside ? 90 : 255), 1));
            p.setBrush(fill);
            drawDiamond(p, QPointF(x, cy), radius);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(Qt::NoBrush);
    }
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

void KeyframeTrackWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    const Hit hit = hitTest(pos);
    const QVector<Row> all = rows();
    m_pressPos = pos;
    if (hit.row >= 0 && hit.frame) {
        clickSelect(refsAt(all[hit.row], *hit.frame), event->modifiers());
        m_pressFrame = *hit.frame;
        m_mode = Mode::Pending;
        return;
    }
    if (hit.row >= 0 && pos.x() < kHeaderWidth) {
        // Label: every key of the row.
        const ClipData* c = clip();
        if (c) {
            const Row& row = all[hit.row];
            clickSelect(ko::keysInRange(*c, std::numeric_limits<int64_t>::min() / 2,
                            std::numeric_limits<int64_t>::max() / 2, row.paths),
                event->modifiers());
        }
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

void KeyframeTrackWidget::mouseMoveEvent(QMouseEvent* event)
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
        const int64_t raw = std::llround(localAtX(pos.x()) - localAtX(m_pressPos.x()));
        int64_t delta = snapLocal(m_pressFrame + raw) - m_pressFrame;
        delta = ko::clampMoveDelta(dragOrigin(), m_dragKeys, delta);
        TimelineData& data = restoreDragOrigin();
        const QVector<KeyframeRef> moved = ko::moveKeys(data, m_dragKeys, delta);
        notifyDragChanged();
        m_ctx.selection->setKeyframes(moved);
        return;
    }
    if (m_mode == Mode::RubberBand) {
        m_band = QRect(m_pressPos, pos).normalized();
        const ClipData* c = clip();
        if (!c)
            return;
        const QVector<Row> all = rows();
        const int r0 = std::max(0, rowAtY(m_band.top()));
        const int r1 = std::min(int(all.size()) - 1, rowAtY(m_band.bottom()));
        const int64_t f0 = int64_t(std::ceil(localAtX(m_band.left())));
        const int64_t f1 = int64_t(std::floor(localAtX(m_band.right())));
        QVector<KeyframeRef> refs = m_baseSelection;
        for (int r = r0; r <= r1; ++r) {
            for (const KeyframeRef& ref : ko::keysInRange(*c, f0, f1, all[r].paths)) {
                if (!refs.contains(ref))
                    refs.append(ref);
            }
        }
        setSelection(refs);
        update();
    }
}

void KeyframeTrackWidget::mouseReleaseEvent(QMouseEvent* event)
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

void KeyframeTrackWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    const Hit hit = hitTest(event->position().toPoint());
    if (hit.row >= 0 && hit.frame)
        jumpTo(*hit.frame);
}

void KeyframeTrackWidget::contextMenuEvent(QContextMenuEvent* event)
{
    const Hit hit = hitTest(event->pos());
    if (hit.row >= 0 && hit.frame) {
        const QVector<KeyframeRef> refs = refsAt(rows()[hit.row], *hit.frame);
        const bool anySelected = std::any_of(
            refs.cbegin(), refs.cend(), [this](const KeyframeRef& r) { return isSelected(r); });
        if (!anySelected)
            clickSelect(refs, Qt::NoModifier);
    }
    showKeyMenu(event->globalPos());
}

} // namespace kk
