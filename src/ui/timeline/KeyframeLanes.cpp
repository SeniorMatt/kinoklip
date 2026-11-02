// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeLanes implementation: editor state, lane geometry, clip toggle buttons and the
// keyframe commands.
#include "ui/timeline/KeyframeLanes.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/timeline/CurveEditorWidget.h"
#include "ui/timeline/KeyframeTrackWidget.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"

#include <QAction>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QWidget>

namespace kk {

using namespace timelinemetrics;
namespace ko = keyframeops;

namespace {
constexpr int kButtonW = 14; ///< clip toggle buttons (keyframes, curves)
constexpr int kButtonH = 12;
constexpr int kButtonGap = 2;
constexpr int kMinClipWidthForButtons = 2 * kButtonW + 3 * kButtonGap + 8;
constexpr int kMinClipHeightForButtons = 26;
const QColor kButtonOn(0xff, 0xa8, 0x3a); ///< timeline selection colour: editor open
constexpr int kOpenPanelGap = 6; ///< gap between the clip and its "open in panel" button

bool clipIsAnimated(const ClipData& c)
{
    if (c.transform.isAnimated() || c.audio.isAnimated() || c.title.isAnimated())
        return true;
    return std::any_of(c.effects.cbegin(), c.effects.cend(),
        [](const EffectInstance& fx) { return fx.params.isAnimated(); });
}
} // namespace

KeyframeLanes::KeyframeLanes(AppContext& ctx, const TimelineLayout& layout,
    TimelineController* controller, QWidget* viewport, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_layout(layout)
    , m_controller(controller)
    , m_viewport(viewport)
    , m_timeMap(layout)
{
    if (ActionRegistry* actions = ctx.actions) {
        auto bind = [&](const char* id, auto fn) {
            if (QAction* a = actions->action(QLatin1String(id)))
                connect(a, &QAction::triggered, this, fn);
        };
        bind("clip.toggle_keyframe_editor", [this] { toggleForSelection(true); });
        bind("clip.toggle_curve_editor", [this] { toggleForSelection(false); });
        bind("clip.delete_keyframe", [this] { deleteSelectedKeys(); });
        bind("clip.copy_keyframes", [this] { copySelectedKeys(); });
        bind("clip.paste_keyframes", [this] { pasteKeys(); });
    }
}

KeyframeLanes::~KeyframeLanes() = default;

double KeyframeLanes::LayoutTimeMap::frameToX(double frame) const
{
    return m_layout.frameToX(frame);
}

double KeyframeLanes::LayoutTimeMap::xToFrame(double x) const
{
    return m_layout.xToFrame(x);
}

double KeyframeLanes::LayoutTimeMap::pixelsPerFrame() const
{
    return m_layout.pixelsPerFrame();
}

void KeyframeLanes::setupLaneWidget(KeyframeViewBase* w, quint64 clipId)
{
    w->setSnappingProvider([this] { return m_controller && m_controller->snapping(); });
    w->setMenuExtender([this, clipId](QMenu& menu) {
        connect(menu.addAction(tr("Open in Keyframes Panel")), &QAction::triggered, this,
            [this, clipId] { emit openInPanelRequested(clipId); });
    });
}

QToolButton* KeyframeLanes::openPanelButton(quint64 clipId) const
{
    return m_lanes.value(clipId).openPanel;
}

void KeyframeLanes::placeOpenPanelButton(
    Lane& l, quint64 clipId, const ClipData& clip, const QRect& lane)
{
    const double x0 = m_layout.frameToX(double(clip.start));
    const double x1 = m_layout.frameToX(double(clip.start + clip.duration));
    const bool narrow = x1 - x0 < kMinUsableClipWidthPx;
    if (!narrow || lane.isEmpty()) {
        if (l.openPanel)
            l.openPanel->hide();
        return;
    }
    if (!l.openPanel) {
        l.openPanel = new QToolButton(m_viewport);
        l.openPanel->setObjectName(QStringLiteral("OpenInKeyframesPanel"));
        l.openPanel->setText(tr("Open in Keyframes Panel"));
        l.openPanel->setToolTip(
            tr("The clip is too narrow to edit its keyframes here: edit them in the Keyframes "
               "panel"));
        l.openPanel->setFocusPolicy(Qt::NoFocus);
        connect(l.openPanel, &QToolButton::clicked, this,
            [this, clipId] { emit openInPanelRequested(clipId); });
    }
    QToolButton* b = l.openPanel;
    b->adjustSize();
    // Right of the clip when there is room, else left of it; at the top of the lane.
    int x = int(x1) + kOpenPanelGap;
    if (x + b->width() > m_viewport->width())
        x = int(x0) - kOpenPanelGap - b->width();
    x = std::max(x, kHeaderWidth + kOpenPanelGap);
    const int y = lane.top() + kButtonGap * 2;
    if (y < kRulerHeight || y + b->height() > m_viewport->height()) {
        b->hide();
        return;
    }
    b->move(x, y);
    b->show();
    b->raise();
}

// ---- State ----

bool KeyframeLanes::isKeyframeEditorOpen(quint64 clipId) const
{
    const auto it = m_states.constFind(clipId);
    return it != m_states.cend() && it->keyframes;
}

bool KeyframeLanes::isCurveEditorOpen(quint64 clipId) const
{
    const auto it = m_states.constFind(clipId);
    return it != m_states.cend() && it->curves;
}

void KeyframeLanes::setKeyframeEditorOpen(quint64 clipId, bool open)
{
    setOpen(clipId, true, open);
}

void KeyframeLanes::setCurveEditorOpen(quint64 clipId, bool open)
{
    setOpen(clipId, false, open);
}

void KeyframeLanes::setOpen(quint64 clipId, bool keyframes, bool open)
{
    if (clipId == 0)
        return;
    ClipState& s = m_states[clipId];
    (keyframes ? s.keyframes : s.curves) = open;
    if (!s.keyframes && !s.curves) {
        m_states.remove(clipId);
        dropLane(clipId);
    } else if (open) {
        s.order = ++m_order;
    }
    emit layoutChanged();
}

void KeyframeLanes::toggleForSelection(bool keyframes)
{
    const quint64 id = m_ctx.selection->currentClip();
    if (id == 0)
        return;
    setOpen(id, keyframes, keyframes ? !isKeyframeEditorOpen(id) : !isCurveEditorOpen(id));
}

void KeyframeLanes::dropLane(quint64 clipId)
{
    const Lane lane = m_lanes.take(clipId);
    if (lane.keys)
        lane.keys->deleteLater();
    if (lane.curves)
        lane.curves->deleteLater();
    if (lane.openPanel)
        lane.openPanel->deleteLater();
}

int KeyframeLanes::keyTrackHeight(const ClipData& clip) const
{
    return KeyframeTrackWidget::heightFor(clip);
}

int KeyframeLanes::curveHeight(const ClipState& s) const
{
    return s.curveHeight > 0 ? s.curveHeight : CurveEditorWidget::kDefaultHeight;
}

QHash<quint64, int> KeyframeLanes::laneHeights(const TimelineData& data)
{
    QHash<quint64, int> heights;
    QHash<quint64, std::pair<quint64, quint64>> owner; // track id -> (order, clip id)
    QVector<quint64> drop;
    for (auto it = m_states.cbegin(); it != m_states.cend(); ++it) {
        const auto loc = editops::findClip(data, it.key());
        const TrackData* track = loc ? data.track(loc->track) : nullptr;
        if (!track) {
            drop.append(it.key()); // the clip was deleted
            continue;
        }
        // One expanded clip per track: the most recently opened wins.
        const auto o = owner.constFind(track->id);
        if (o != owner.cend()) {
            if (o->first > it->order) {
                drop.append(it.key());
                continue;
            }
            drop.append(o->second);
        }
        owner.insert(track->id, {it->order, it.key()});
    }
    for (quint64 id : drop) {
        m_states.remove(id);
        dropLane(id);
    }
    for (auto o = owner.cbegin(); o != owner.cend(); ++o) {
        const ClipData* clip = editops::clipById(data, o->second);
        const ClipState& s = m_states[o->second];
        heights.insert(o.key(),
            (s.keyframes && clip ? keyTrackHeight(*clip) : 0) + (s.curves ? curveHeight(s) : 0));
    }
    return heights;
}

void KeyframeLanes::syncGeometry()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    for (auto it = m_states.cbegin(); it != m_states.cend(); ++it) {
        const quint64 id = it.key();
        const auto loc = editops::findClip(data, id);
        const ClipData* clip = editops::clipById(data, id);
        const int row = loc ? m_layout.rowOf(loc->track) : -1;
        const QRect lane = m_layout.laneRect(row);
        Lane& l = m_lanes[id];
        if (lane.isEmpty() || !clip) {
            if (l.keys)
                l.keys->hide();
            if (l.curves)
                l.curves->hide();
            if (l.openPanel)
                l.openPanel->hide();
            continue;
        }
        const int keysH = it->keyframes ? keyTrackHeight(*clip) : 0;
        if (it->keyframes) {
            if (!l.keys) {
                l.keys = new KeyframeTrackWidget(m_ctx, m_timeMap, id, m_viewport);
                setupLaneWidget(l.keys, id);
            }
            l.keys->placeLane(QRect(lane.left(), lane.top(), lane.width(), keysH), kRulerHeight);
            l.keys->update();
        } else if (l.keys) {
            l.keys->deleteLater();
            l.keys = nullptr;
        }
        if (it->curves) {
            if (!l.curves) {
                l.curves = new CurveEditorWidget(m_ctx, m_timeMap, id, m_viewport);
                setupLaneWidget(l.curves, id);
            }
            l.curves->placeLane(
                QRect(lane.left(), lane.top() + keysH, lane.width(), lane.height() - keysH),
                kRulerHeight);
            l.curves->update();
        } else if (l.curves) {
            l.curves->deleteLater();
            l.curves = nullptr;
        }
        placeOpenPanelButton(l, id, *clip, lane);
    }
}

KeyframeTrackWidget* KeyframeLanes::keyframeWidget(quint64 clipId) const
{
    return m_lanes.value(clipId).keys;
}

CurveEditorWidget* KeyframeLanes::curveWidget(quint64 clipId) const
{
    return m_lanes.value(clipId).curves;
}

// ---- Clip toggle buttons ----

std::pair<QRect, QRect> KeyframeLanes::clipButtonRects(int row, const ClipData& clip) const
{
    const QRectF r = m_layout.clipRect(row, clip);
    const double left = std::max(r.left(), double(kHeaderWidth));
    const double right = std::min(r.right(), double(m_layout.viewSize().width()));
    if (right - left < kMinClipWidthForButtons || r.height() < kMinClipHeightForButtons)
        return {};
    // Bottom-right corner of the visible part of the clip: curves outermost, keyframes left.
    const int y = int(r.bottom()) - kButtonH - kButtonGap;
    const int xCurves = int(right) - kButtonW - kButtonGap - 1;
    const QRect curves(xCurves, y, kButtonW, kButtonH);
    const QRect keys(xCurves - kButtonW - kButtonGap, y, kButtonW, kButtonH);
    return {keys, curves};
}

void KeyframeLanes::paintClipButtons(QPainter& p) const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const auto [f0, f1] = m_layout.visibleFrames();
    const auto& rows = m_layout.rows();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int row = 0; row < int(rows.size()); ++row) {
        const QRect rr = m_layout.rowRect(row);
        if (rr.bottom() < kRulerHeight || rr.top() > m_layout.viewSize().height())
            continue;
        const TrackData* track = data.track(rows[size_t(row)].ref);
        if (!track)
            continue;
        const auto [first, last] = TimelineLayout::clipRange(*track, double(f0), double(f1 + 1));
        for (size_t i = first; i < last; ++i) {
            const ClipData& clip = track->clips[i];
            const bool keysOpen = isKeyframeEditorOpen(clip.id);
            const bool curvesOpen = isCurveEditorOpen(clip.id);
            if (!keysOpen && !curvesOpen && !clipIsAnimated(clip))
                continue;
            const auto [keys, curves] = clipButtonRects(row, clip);
            if (keys.isEmpty() || keys.top() < kRulerHeight)
                continue;
            for (const bool isCurve : {false, true}) {
                const QRect b = isCurve ? curves : keys;
                const bool on = isCurve ? curvesOpen : keysOpen;
                p.setPen(QColor(0, 0, 0, 160));
                p.setBrush(on ? kButtonOn : QColor(20, 20, 24, 150));
                p.drawRoundedRect(QRectF(b).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
                const QColor glyph = on ? QColor(20, 20, 20) : QColor(235, 235, 235);
                const QPointF c = QRectF(b).center();
                if (isCurve) {
                    QPainterPath path;
                    path.moveTo(c.x() - 4.5, c.y() + 3);
                    path.cubicTo(
                        c.x() - 0.5, c.y() + 3, c.x() + 0.5, c.y() - 3, c.x() + 4.5, c.y() - 3);
                    p.setPen(QPen(glyph, 1.3));
                    p.setBrush(Qt::NoBrush);
                    p.drawPath(path);
                } else {
                    const QPointF pts[4] = {{c.x(), c.y() - 3.5}, {c.x() + 3.5, c.y()},
                        {c.x(), c.y() + 3.5}, {c.x() - 3.5, c.y()}};
                    p.setPen(Qt::NoPen);
                    p.setBrush(glyph);
                    p.drawConvexPolygon(pts, 4);
                }
            }
        }
    }
    p.restore();
}

bool KeyframeLanes::clickClipButton(QPointF pos)
{
    const TimelineHit hit = m_layout.hitTest(m_ctx.project->timeline()->data(), pos);
    if (hit.area != TimelineHit::Area::Clip || hit.clipId == 0)
        return false;
    const ClipData* clip = editops::clipById(m_ctx.project->timeline()->data(), hit.clipId);
    if (!clip)
        return false;
    if (!clipIsAnimated(*clip) && !isKeyframeEditorOpen(clip->id) && !isCurveEditorOpen(clip->id))
        return false;
    const auto [keys, curves] = clipButtonRects(hit.row, *clip);
    const QPoint p = pos.toPoint();
    if (keys.contains(p)) {
        setKeyframeEditorOpen(clip->id, !isKeyframeEditorOpen(clip->id));
        return true;
    }
    if (curves.contains(p)) {
        setCurveEditorOpen(clip->id, !isCurveEditorOpen(clip->id));
        return true;
    }
    return false;
}

// ---- Commands ----

bool KeyframeLanes::deleteSelectedKeys()
{
    return keyframecommands::deleteSelected(m_ctx);
}

bool KeyframeLanes::copySelectedKeys()
{
    return keyframecommands::copySelected(m_ctx);
}

bool KeyframeLanes::hasCopiedKeys() const
{
    return keyframecommands::hasCopied();
}

bool KeyframeLanes::pasteKeys()
{
    return keyframecommands::paste(m_ctx);
}

bool KeyframeLanes::setSelectedInterp(Interp interp)
{
    return keyframecommands::setSelectedInterp(m_ctx, interp);
}

bool KeyframeLanes::flattenSelected()
{
    return keyframecommands::flattenSelected(m_ctx);
}

} // namespace kk
