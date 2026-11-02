// SPDX-License-Identifier: GPL-3.0-or-later
// ClipLaneWidget implementation: geometry, selection, drag protocol, menu, keyboard.
#include "ui/timeline/ClipLaneWidget.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"

#include <QActionGroup>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>

#include <cmath>

namespace kk {

using namespace timelinemetrics;

namespace {
constexpr int kSnapTolerancePx = 8; ///< same tolerance as clip snapping on the timeline
const QColor kPlayheadColor(0xe5, 0x48, 0x4d); ///< matches TimelinePainter's playhead

QColor mix(const QColor& a, const QColor& b, double t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t),
        float(a.greenF() + (b.greenF() - a.greenF()) * t),
        float(a.blueF() + (b.blueF() - a.blueF()) * t));
}
} // namespace

ClipLaneWidget::ClipLaneWidget(AppContext& ctx, const TimelineLayout& layout,
    TimelineController* controller, KeyframeLanes* lanes, quint64 clipId, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
    , m_layout(layout)
    , m_controller(controller)
    , m_lanes(lanes)
    , m_clipId(clipId)
{
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, [this] { update(); });
    connect(ctx.selection, &SelectionModel::changed, this, [this] { update(); });
}

ClipLaneWidget::~ClipLaneWidget()
{
    if (m_editOpen)
        m_ctx.project->cancelEdit();
}

void ClipLaneWidget::placeLane(const QRect& lane, int minTop)
{
    m_laneHeight = lane.height();
    const QRect visible = lane.intersected(QRect(lane.left(), minTop, lane.width(), 1 << 20));
    if (visible.isEmpty()) {
        hide();
        return;
    }
    const int offset = visible.top() - lane.top();
    const bool moved = offset != m_yOffset || visible != geometry();
    m_yOffset = offset;
    setGeometry(visible);
    if (moved)
        update();
    show();
}

const ClipData* ClipLaneWidget::clip() const
{
    return editops::clipById(m_ctx.project->timeline()->data(), m_clipId);
}

double ClipLaneWidget::xOfLocal(double localFrame) const
{
    const ClipData* c = clip();
    return m_layout.frameToX(double(c ? c->start : 0) + localFrame);
}

double ClipLaneWidget::localAtX(double x) const
{
    const ClipData* c = clip();
    return m_layout.xToFrame(x) - double(c ? c->start : 0);
}

double ClipLaneWidget::pixelsPerFrame() const
{
    return m_layout.pixelsPerFrame();
}

// ---- Selection ----

QVector<KeyframeRef> ClipLaneWidget::selectedKeys() const
{
    QVector<KeyframeRef> out;
    for (const KeyframeRef& r : m_ctx.selection->keyframes()) {
        if (r.clipId == m_clipId)
            out.append(r);
    }
    return out;
}

bool ClipLaneWidget::isSelected(const KeyframeRef& ref) const
{
    return m_ctx.selection->keyframes().contains(ref);
}

void ClipLaneWidget::setSelection(const QVector<KeyframeRef>& refs)
{
    if (!m_ctx.selection->isSelected(m_clipId))
        m_ctx.selection->selectClip(m_clipId); // clears keys of other clips too
    m_ctx.selection->setKeyframes(refs);
}

void ClipLaneWidget::clickSelect(const QVector<KeyframeRef>& refs, Qt::KeyboardModifiers modifiers)
{
    QVector<KeyframeRef> current = m_ctx.selection->isSelected(m_clipId)
        ? m_ctx.selection->keyframes()
        : QVector<KeyframeRef> {};
    if (modifiers & Qt::ControlModifier) {
        // Toggle: if every ref is selected remove them, else add the missing ones.
        const bool all = std::all_of(
            refs.cbegin(), refs.cend(), [&](const KeyframeRef& r) { return current.contains(r); });
        for (const KeyframeRef& r : refs) {
            if (all)
                current.removeAll(r);
            else if (!current.contains(r))
                current.append(r);
        }
    } else if (modifiers & Qt::ShiftModifier) {
        for (const KeyframeRef& r : refs) {
            if (!current.contains(r))
                current.append(r);
        }
    } else {
        const bool all = !refs.isEmpty()
            && std::all_of(refs.cbegin(), refs.cend(),
                [&](const KeyframeRef& r) { return current.contains(r); });
        if (!all)
            current = refs;
    }
    setSelection(current);
}

// ---- Drag / undo ----

void ClipLaneWidget::beginDragEdit(const QString& label)
{
    if (m_editOpen)
        return;
    m_origin = m_ctx.project->timeline()->data();
    m_ctx.project->beginEdit(label);
    m_editOpen = true;
}

TimelineData& ClipLaneWidget::restoreDragOrigin()
{
    TimelineData& data = m_ctx.project->timeline()->mutableData();
    data = m_origin;
    return data;
}

void ClipLaneWidget::notifyDragChanged()
{
    m_ctx.project->timeline()->notifyChanged();
}

void ClipLaneWidget::endDragEdit(bool commit)
{
    if (!m_editOpen)
        return;
    m_editOpen = false;
    if (commit)
        m_ctx.project->endEdit();
    else
        m_ctx.project->cancelEdit();
    m_origin = TimelineData();
}

int64_t ClipLaneWidget::snapLocal(int64_t localFrame) const
{
    const ClipData* c = clip();
    if (!c || !m_controller || !m_controller->snapping())
        return localFrame;
    const double tolerance = kSnapTolerancePx / std::max(1e-6, pixelsPerFrame());
    const int64_t candidates[]
        = {m_ctx.project->timeline()->playhead() - c->start, 0, c->duration - 1};
    int64_t best = localFrame;
    double bestDistance = tolerance;
    for (int64_t f : candidates) {
        const double d = std::abs(double(f - localFrame));
        if (d <= bestDistance) {
            bestDistance = d;
            best = f;
        }
    }
    return best;
}

void ClipLaneWidget::jumpTo(int64_t localFrame)
{
    if (const ClipData* c = clip())
        m_ctx.project->timeline()->setPlayhead(c->start + localFrame);
}

// ---- Menu ----

void ClipLaneWidget::showKeyMenu(const QPoint& globalPos)
{
    const QVector<KeyframeRef> keys = selectedKeys();
    const ClipData* c = clip();
    // Preset shared by every selected key (checked in the menu), if any.
    std::optional<Interp> common;
    bool mixed = false;
    if (c) {
        for (const KeyframeRef& r : keys) {
            const AnimatedValue* v = keyframeops::findValue(*c, r.paramPath);
            const qsizetype i = v ? v->indexOf(r.frame) : -1;
            if (i < 0)
                continue;
            const Interp it = keyInterp(v->keys[i]);
            if (common && *common != it)
                mixed = true;
            common = it;
        }
    }
    QMenu menu(this);
    auto* group = new QActionGroup(&menu);
    const std::pair<Interp, QString> presets[] = {
        {Interp::Linear, tr("Linear")},
        {Interp::EaseIn, tr("Ease In")},
        {Interp::EaseOut, tr("Ease Out")},
        {Interp::EaseInOut, tr("Ease In and Out")},
        {Interp::Smooth, tr("Smooth (Bezier)")},
        {Interp::Hold, tr("Static (Hold)")},
    };
    for (const auto& [mode, text] : presets) {
        QAction* a = menu.addAction(text);
        a->setCheckable(true);
        a->setChecked(!mixed && common == mode);
        a->setEnabled(!keys.isEmpty());
        group->addAction(a);
        const Interp m = mode;
        connect(a, &QAction::triggered, this, [this, m] { m_lanes->setSelectedInterp(m); });
    }
    menu.addSeparator();
    QAction* copy = menu.addAction(tr("Copy Keyframes"));
    copy->setEnabled(!keys.isEmpty());
    connect(copy, &QAction::triggered, this, [this] { m_lanes->copySelectedKeys(); });
    QAction* paste = menu.addAction(tr("Paste Keyframes at Playhead"));
    paste->setEnabled(m_lanes->hasCopiedKeys());
    connect(paste, &QAction::triggered, this, [this] { m_lanes->pasteKeys(); });
    QAction* del = menu.addAction(tr("Delete Keyframes"));
    del->setEnabled(!keys.isEmpty());
    connect(del, &QAction::triggered, this, [this] { m_lanes->deleteSelectedKeys(); });
    menu.exec(globalPos);
}

// ---- Painting ----

void ClipLaneWidget::paintLaneBackground(QPainter& p, const QRect& logicalRect) const
{
    const QPalette& pal = palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::WindowText);
    // Track area: a shade darker than the lanes so the editor reads as a separate panel.
    p.fillRect(logicalRect, mix(base, QColor(0, 0, 0), 0.12));
    p.fillRect(QRect(logicalRect.left(), logicalRect.top(), kHeaderWidth, logicalRect.height()),
        mix(pal.color(QPalette::Window), text, 0.03));
    p.setPen(pal.color(QPalette::Dark));
    p.drawLine(kHeaderWidth - 1, logicalRect.top(), kHeaderWidth - 1, logicalRect.bottom());
    p.drawLine(logicalRect.left(), logicalRect.top(), logicalRect.right(), logicalRect.top());
    // The clip's own span is lighter: keys outside it (trimmed away) still exist but fade.
    if (const ClipData* c = clip()) {
        const double x0 = std::max(double(kHeaderWidth), xOfLocal(0));
        const double x1 = std::min(double(width()), xOfLocal(double(c->duration)));
        if (x1 > x0)
            p.fillRect(QRectF(x0, logicalRect.top() + 1, x1 - x0, logicalRect.height() - 1),
                mix(base, text, 0.04));
    }
}

void ClipLaneWidget::paintPlayhead(QPainter& p) const
{
    const double x
        = std::round(m_layout.frameToX(double(m_ctx.project->timeline()->playhead()))) + 0.5;
    if (x < kHeaderWidth || x > width())
        return;
    p.setPen(QPen(kPlayheadColor, 1));
    p.drawLine(QPointF(x, m_yOffset), QPointF(x, m_yOffset + height()));
}

void ClipLaneWidget::drawDiamond(QPainter& p, QPointF c, double r)
{
    const QPointF pts[4]
        = {{c.x(), c.y() - r}, {c.x() + r, c.y()}, {c.x(), c.y() + r}, {c.x() - r, c.y()}};
    p.drawConvexPolygon(pts, 4);
}

// ---- Keyboard ----

bool ClipLaneWidget::event(QEvent* event)
{
    // With keys selected in this lane, Delete / Backspace and Ctrl+C / Ctrl+V act on the keys
    // (like Resolve) instead of the clip commands bound to the same keys.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(event);
        const bool hasKeys = !selectedKeys().isEmpty();
        if ((ke->matches(QKeySequence::Delete) || ke->key() == Qt::Key_Backspace
                || ke->key() == Qt::Key_Delete || ke->matches(QKeySequence::Copy))
            && hasKeys) {
            event->accept();
            return true;
        }
        if (ke->matches(QKeySequence::Paste) && m_lanes->hasCopiedKeys()) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void ClipLaneWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_editOpen) {
        endDragEdit(false);
        update();
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_lanes->deleteSelectedKeys())
            return;
    } else if (event->matches(QKeySequence::Copy)) {
        if (m_lanes->copySelectedKeys())
            return;
    } else if (event->matches(QKeySequence::Paste)) {
        if (m_lanes->pasteKeys())
            return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace kk
