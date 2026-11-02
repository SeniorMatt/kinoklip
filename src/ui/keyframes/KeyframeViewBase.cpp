// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeViewBase implementation: clips, selection, drag protocol, snapping, menu, keyboard.
#include "ui/keyframes/KeyframeViewBase.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframeTimeMap.h"

#include <QAction>
#include <QActionGroup>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace kk {

namespace ko = keyframeops;
namespace kc = keyframecommands;

namespace {
constexpr int kSnapTolerancePx = 8; ///< same tolerance as clip snapping on the timeline
const QColor kPlayheadColor(0xe5, 0x48, 0x4d); ///< matches TimelinePainter's playhead
} // namespace

QColor keyframeMix(const QColor& a, const QColor& b, double t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t),
        float(a.greenF() + (b.greenF() - a.greenF()) * t),
        float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

KeyframeViewBase::KeyframeViewBase(AppContext& ctx, const KeyframeTimeMap& map, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
    , m_map(map)
{
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, [this] { update(); });
    connect(ctx.selection, &SelectionModel::changed, this, [this] { update(); });
    m_snapping = [this] {
        const QAction* a
            = m_ctx.actions && m_ctx.actions->contains(QStringLiteral("timeline.snapping"))
            ? m_ctx.actions->action(QStringLiteral("timeline.snapping"))
            : nullptr;
        return a && a->isChecked();
    };
}

KeyframeViewBase::~KeyframeViewBase()
{
    if (m_editOpen)
        m_ctx.project->cancelEdit();
}

void KeyframeViewBase::setClips(const QVector<quint64>& clipIds)
{
    if (clipIds == m_clips)
        return;
    m_clips = clipIds;
    update();
}

void KeyframeViewBase::setLabelWidth(int px)
{
    m_labelWidth = std::max(0, px);
    update();
}

void KeyframeViewBase::setVerticalWindow(int yOffset, int logicalHeight)
{
    if (yOffset == m_yOffset && logicalHeight == m_logicalHeight)
        return;
    m_yOffset = yOffset;
    m_logicalHeight = logicalHeight;
    update();
}

const ClipData* KeyframeViewBase::clipData(quint64 clipId) const
{
    return editops::clipById(m_ctx.project->timeline()->data(), clipId);
}

const ClipData* KeyframeViewBase::primaryClip() const
{
    return m_clips.isEmpty() ? nullptr : clipData(m_clips.front());
}

double KeyframeViewBase::xOfLocal(const ClipData& clip, double localFrame) const
{
    return m_map.frameToX(double(clip.start) + localFrame);
}

double KeyframeViewBase::frameAtX(double x) const
{
    return m_map.xToFrame(x);
}

double KeyframeViewBase::pixelsPerFrame() const
{
    return m_map.pixelsPerFrame();
}

QVector<KeyframeRef> KeyframeViewBase::keysAtFrame(
    int64_t frame, const QVector<ParamId>& params) const
{
    QVector<KeyframeRef> refs;
    for (const ParamId& id : params) {
        const ClipData* c = clipData(id.clipId);
        const AnimatedValue* v = c ? ko::findValue(*c, id.path) : nullptr;
        if (v && v->hasKeyAt(frame - c->start))
            refs.append({id.clipId, id.path, frame - c->start});
    }
    return refs;
}

// ---- Selection ----

QVector<KeyframeRef> KeyframeViewBase::selectedKeys() const
{
    QVector<KeyframeRef> out;
    for (const KeyframeRef& r : m_ctx.selection->keyframes()) {
        if (m_clips.contains(r.clipId))
            out.append(r);
    }
    return out;
}

bool KeyframeViewBase::isSelected(const KeyframeRef& ref) const
{
    return m_ctx.selection->keyframes().contains(ref);
}

void KeyframeViewBase::setSelection(const QVector<KeyframeRef>& refs)
{
    const bool anyShownSelected = std::any_of(m_clips.cbegin(), m_clips.cend(),
        [this](quint64 id) { return m_ctx.selection->isSelected(id); });
    if (!anyShownSelected && !m_clips.isEmpty())
        m_ctx.selection->selectClip(m_clips.front()); // clears keys of other clips too
    m_ctx.selection->setKeyframes(refs);
}

void KeyframeViewBase::clickSelect(
    const QVector<KeyframeRef>& refs, Qt::KeyboardModifiers modifiers)
{
    const bool anyShownSelected = std::any_of(m_clips.cbegin(), m_clips.cend(),
        [this](quint64 id) { return m_ctx.selection->isSelected(id); });
    QVector<KeyframeRef> current
        = anyShownSelected ? m_ctx.selection->keyframes() : QVector<KeyframeRef> {};
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

void KeyframeViewBase::beginDragEdit(const QString& label)
{
    if (m_editOpen)
        return;
    m_origin = m_ctx.project->timeline()->data();
    m_ctx.project->beginEdit(label);
    m_editOpen = true;
}

TimelineData& KeyframeViewBase::restoreDragOrigin()
{
    TimelineData& data = m_ctx.project->timeline()->mutableData();
    data = m_origin;
    return data;
}

void KeyframeViewBase::notifyDragChanged()
{
    m_ctx.project->timeline()->notifyChanged();
}

void KeyframeViewBase::endDragEdit(bool commit)
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

int64_t KeyframeViewBase::snapFrame(int64_t frame) const
{
    if (!m_snapping || !m_snapping())
        return frame;
    const double tolerance = kSnapTolerancePx / std::max(1e-6, pixelsPerFrame());
    int64_t best = frame;
    double bestDistance = tolerance;
    auto consider = [&](int64_t f) {
        const double d = std::abs(double(f - frame));
        if (d <= bestDistance) {
            bestDistance = d;
            best = f;
        }
    };
    consider(m_ctx.project->timeline()->playhead());
    for (quint64 id : m_clips) {
        if (const ClipData* c = clipData(id)) {
            consider(c->start);
            consider(c->start + c->duration - 1);
        }
    }
    return best;
}

void KeyframeViewBase::jumpToFrame(int64_t frame)
{
    m_ctx.project->timeline()->setPlayhead(std::max<int64_t>(0, frame));
}

// ---- Menu ----

void KeyframeViewBase::showKeyMenu(const QPoint& globalPos)
{
    const QVector<KeyframeRef> keys = selectedKeys();
    // Preset shared by every selected key (checked in the menu), if any.
    std::optional<Interp> common;
    bool mixed = false;
    for (const KeyframeRef& r : keys) {
        const ClipData* c = clipData(r.clipId);
        const AnimatedValue* v = c ? ko::findValue(*c, r.paramPath) : nullptr;
        const qsizetype i = v ? v->indexOf(r.frame) : -1;
        if (i < 0)
            continue;
        const Interp it = keyInterp(v->keys[i]);
        if (common && *common != it)
            mixed = true;
        common = it;
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
        connect(a, &QAction::triggered, this, [this, m] { kc::setSelectedInterp(m_ctx, m); });
    }
    QAction* flatten = menu.addAction(tr("Flatten Handles"));
    flatten->setEnabled(!keys.isEmpty());
    connect(flatten, &QAction::triggered, this, [this] { kc::flattenSelected(m_ctx); });
    menu.addSeparator();
    QAction* copy = menu.addAction(tr("Copy Keyframes"));
    copy->setEnabled(!keys.isEmpty());
    connect(copy, &QAction::triggered, this, [this] { kc::copySelected(m_ctx); });
    QAction* paste = menu.addAction(tr("Paste Keyframes at Playhead"));
    paste->setEnabled(kc::hasCopied());
    connect(paste, &QAction::triggered, this, [this] { kc::paste(m_ctx); });
    QAction* del = menu.addAction(tr("Delete Keyframes"));
    del->setEnabled(!keys.isEmpty());
    connect(del, &QAction::triggered, this, [this] { kc::deleteSelected(m_ctx); });
    if (m_menuExtender) {
        menu.addSeparator();
        m_menuExtender(menu);
    }
    menu.exec(globalPos);
}

// ---- Painting ----

void KeyframeViewBase::paintBackground(QPainter& p, const QRect& logicalRect) const
{
    const QPalette& pal = palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::WindowText);
    // Data area: a shade darker than the lanes so the editor reads as a separate panel.
    p.fillRect(logicalRect, keyframeMix(base, QColor(0, 0, 0), 0.12));
    if (m_labelWidth > 0) {
        p.fillRect(QRect(logicalRect.left(), logicalRect.top(), m_labelWidth, logicalRect.height()),
            keyframeMix(pal.color(QPalette::Window), text, 0.03));
        p.setPen(pal.color(QPalette::Dark));
        p.drawLine(m_labelWidth - 1, logicalRect.top(), m_labelWidth - 1, logicalRect.bottom());
    }
    // The spans of the shown clips are lighter: keys outside them (trimmed away) still exist
    // but fade. Their edges are marked so clip boundaries read in a wide view too.
    const QColor edge = keyframeMix(base, text, 0.30);
    for (quint64 id : m_clips) {
        const ClipData* c = clipData(id);
        if (!c)
            continue;
        const double cx0 = xOfLocal(*c, 0);
        const double cx1 = xOfLocal(*c, double(c->duration));
        const double x0 = std::max(double(m_labelWidth), cx0);
        const double x1 = std::min(double(width()), cx1);
        if (x1 > x0)
            p.fillRect(QRectF(x0, logicalRect.top(), x1 - x0, logicalRect.height()),
                keyframeMix(base, text, 0.04));
        p.setPen(QPen(edge, 1));
        for (const double x : {cx0, cx1}) {
            if (x >= m_labelWidth && x <= width()) {
                const double xr = std::round(x) + 0.5;
                p.drawLine(QPointF(xr, logicalRect.top()), QPointF(xr, logicalRect.bottom()));
            }
        }
    }
}

void KeyframeViewBase::paintPlayhead(QPainter& p) const
{
    const double x
        = std::round(m_map.frameToX(double(m_ctx.project->timeline()->playhead()))) + 0.5;
    if (x < m_labelWidth || x > width())
        return;
    p.setPen(QPen(kPlayheadColor, 1));
    p.drawLine(QPointF(x, m_yOffset), QPointF(x, m_yOffset + height()));
}

void KeyframeViewBase::paintBand(QPainter& p, const QRectF& band) const
{
    if (band.isEmpty())
        return;
    QColor fill = palette().color(QPalette::Highlight);
    fill.setAlpha(50);
    p.fillRect(band, fill);
    p.setPen(palette().color(QPalette::Highlight));
    p.setBrush(Qt::NoBrush);
    p.drawRect(band.adjusted(0, 0, -1, -1));
}

void KeyframeViewBase::drawDiamond(QPainter& p, QPointF c, double r)
{
    const QPointF pts[4]
        = {{c.x(), c.y() - r}, {c.x() + r, c.y()}, {c.x(), c.y() + r}, {c.x() - r, c.y()}};
    p.drawConvexPolygon(pts, 4);
}

// ---- Keyboard ----

bool KeyframeViewBase::event(QEvent* event)
{
    // With keys selected in this view, Delete / Backspace and Ctrl+C / Ctrl+V act on the keys
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
        if (ke->matches(QKeySequence::Paste) && kc::hasCopied()) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void KeyframeViewBase::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_editOpen) {
        endDragEdit(false);
        update();
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (kc::deleteSelected(m_ctx))
            return;
    } else if (event->matches(QKeySequence::Copy)) {
        if (kc::copySelected(m_ctx))
            return;
    } else if (event->matches(QKeySequence::Paste)) {
        if (kc::paste(m_ctx))
            return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace kk
