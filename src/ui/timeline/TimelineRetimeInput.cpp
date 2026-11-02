// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget, retime controls (Ctrl+R): hit-testing of the speed bar, the speed menu and
// the speed point menu, handle drags (one undo step each) and the retime curve request.
#include "core/KeyframeOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/RetimeOps.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/CurveEditorWidget.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QActionGroup>
#include <QMenu>

#include <cmath>

namespace kk {

namespace {

/// Speeds offered by the speed menu (Resolve-like presets), as factors.
constexpr double kSpeedPresets[]
    = {0.1, 0.25, 0.5, 0.75, 1.0, 1.1, 1.25, 1.5, 2.0, 2.5, 3.0, 5.0, 10.0};

} // namespace

retimebar::Geometry TimelineWidget::retimeGeometry(quint64 clipId) const
{
    if (!m_controller->retimeControlsShown(clipId))
        return {};
    const TimelineData& data = m_ctx.project->timeline()->data();
    const auto loc = editops::findClip(data, clipId);
    if (!loc)
        return {};
    const int row = m_layout.rowOf(loc->track);
    if (row < 0)
        return {};
    const ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
    return retimebar::geometry(clip, m_layout.clipRect(row, clip), m_layout);
}

retimebar::Hit TimelineWidget::retimeHitAt(QPointF pos, quint64* clipId) const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    // A handle may stick out of its clip by half its width: test the clips around pos.
    for (const double dx : {0.0, -retimebar::kHandleHalfWidth, retimebar::kHandleHalfWidth}) {
        const TimelineHit hit = m_layout.hitTest(data, pos + QPointF(dx, 0));
        if (hit.area != TimelineHit::Area::Clip || !m_controller->retimeControlsShown(hit.clipId))
            continue;
        const ClipData* clip = editops::clipById(data, hit.clipId);
        if (!clip)
            continue;
        const retimebar::Hit h = retimebar::hitTest(retimeGeometry(hit.clipId), pos, *clip);
        if (h.kind != retimebar::Hit::Kind::None) {
            *clipId = hit.clipId;
            return h;
        }
    }
    return {};
}

bool TimelineWidget::pressRetime(QPointF pos)
{
    quint64 clipId = 0;
    const retimebar::Hit h = retimeHitAt(pos, &clipId);
    switch (h.kind) {
    case retimebar::Hit::Kind::None:
        return false;
    case retimebar::Hit::Kind::Segment: {
        // Only the arrow opens the menu; the rest of the bar acts like the clip body.
        const retimebar::Geometry g = retimeGeometry(clipId);
        if (h.segment < 0 || h.segment >= g.fields.size()
            || !g.fields[h.segment].arrow.contains(pos))
            return false;
        showSpeedMenu(clipId, h.segment, pos.toPoint());
        return true;
    }
    case retimebar::Hit::Kind::PointTop:
    case retimebar::Hit::Kind::PointBottom:
    case retimebar::Hit::Kind::EndHandle:
        m_drag = Drag {};
        m_drag.pressPos = pos;
        m_drag.lastPos = pos;
        m_drag.retime = h;
        m_drag.retimeClip = clipId;
        m_drag.pending = DragMode::RetimePoint;
        m_drag.mode = DragMode::Pending;
        return true;
    }
    return false;
}

void TimelineWidget::updateRetimeDrag(QPointF pos)
{
    restoreOrigin();
    TimelineData& data = m_ctx.project->timeline()->mutableData();
    const int64_t delta
        = m_layout.nearestFrameAtX(pos.x()) - m_layout.nearestFrameAtX(m_drag.pressPos.x());
    if (delta != 0) {
        if (m_drag.retime.kind == retimebar::Hit::Kind::PointTop)
            (void)retimeops::slidePoint(data, m_drag.retimeClip, m_drag.retime.localFrame, delta);
        else
            (void)retimeops::retimePoint(data, m_drag.retimeClip, m_drag.retime.localFrame, delta);
    }
    m_ctx.project->timeline()->notifyChanged();
}

void TimelineWidget::showSpeedMenu(quint64 clipId, int segment, QPoint pos)
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const ClipData* clip = editops::clipById(data, clipId);
    if (!clip)
        return;
    const QVector<retime::Segment> segs = retime::segments(*clip);
    if (segment < 0 || segment >= segs.size())
        return;
    const retime::Segment seg = segs[segment];
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    const bool playheadInside = clip->contains(playhead);
    TimelineController* tc = m_controller;

    if (m_retimeMenu)
        m_retimeMenu->close();
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("SpeedMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    m_retimeMenu = menu;

    QMenu* change = menu->addMenu(tr("Change Speed"));
    change->setObjectName(QStringLiteral("SpeedMenuPresets"));
    for (const double s : kSpeedPresets) {
        QAction* a = change->addAction(retime::formatPercent(seg.isReverse() ? -s : s), this,
            [tc, clipId, segment, s, reverse = seg.isReverse()] {
                tc->setSegmentSpeed(clipId, segment, reverse ? -s : s);
            });
        a->setCheckable(true);
        a->setChecked(std::abs(std::abs(seg.speed) - s) < 1e-6);
        a->setObjectName(QStringLiteral("SpeedPreset_%1").arg(int(std::lround(s * 100))));
    }
    QAction* normal = menu->addAction(tr("Normal (100%)"), this,
        [tc, clipId, segment] { tc->setSegmentSpeed(clipId, segment, 1.0); });
    normal->setObjectName(QStringLiteral("SpeedNormal"));
    menu->addSeparator();
    QAction* add = menu->addAction(
        tr("Add Speed Point"), this, [tc, clipId] { tc->addSpeedPointAtPlayhead(clipId); });
    add->setObjectName(QStringLiteral("SpeedAddPoint"));
    add->setEnabled(playheadInside && playhead > clip->start);
    QAction* freeze = menu->addAction(
        tr("Freeze Frame"), this, [tc, clipId] { tc->freezeSegmentAtPlayhead(clipId); });
    freeze->setObjectName(QStringLiteral("SpeedFreeze"));
    freeze->setEnabled(playheadInside);
    QAction* reverse = menu->addAction(tr("Reverse Segment"), this,
        [tc, clipId, segment] { tc->reverseSegment(clipId, segment); });
    reverse->setObjectName(QStringLiteral("SpeedReverse"));
    reverse->setEnabled(!seg.isFreeze());
    QAction* reset
        = menu->addAction(tr("Reset Clip"), this, [tc, clipId] { tc->resetClipSpeed(clipId); });
    reset->setObjectName(QStringLiteral("SpeedReset"));
    reset->setEnabled(retime::isRetimed(*clip));
    menu->addSeparator();
    QAction* curve
        = menu->addAction(tr("Retime Curve"), this, [this, clipId] { showRetimeCurve(clipId); });
    curve->setObjectName(QStringLiteral("SpeedCurve"));
    QMenu* process = menu->addMenu(tr("Retime Process"));
    process->setObjectName(QStringLiteral("SpeedProcess"));
    auto* group = new QActionGroup(process);
    const std::pair<RetimeProcess, QString> processes[] = {
        {RetimeProcess::ProjectDefault, tr("Project Settings")},
        {RetimeProcess::Nearest, tr("Nearest")},
        {RetimeProcess::FrameBlend, tr("Frame Blend")},
        {RetimeProcess::OpticalFlow, tr("Optical Flow")},
    };
    for (const auto& [value, text] : processes) {
        QAction* a = process->addAction(
            text, this, [tc, clipId, v = value] { tc->setRetimeProcess(clipId, v); });
        a->setCheckable(true);
        a->setChecked(clip->retimeProcess == value);
        a->setObjectName(QStringLiteral("Process_") + retime::processToString(value));
        group->addAction(a);
    }
    QAction* pitch = menu->addAction(tr("Pitch Correction"));
    pitch->setObjectName(QStringLiteral("SpeedPitch"));
    pitch->setCheckable(true);
    pitch->setChecked(clip->pitchCorrection);
    connect(pitch, &QAction::toggled, this,
        [tc, clipId](bool on) { tc->setPitchCorrection(clipId, on); });
    menu->addSeparator();
    menu->addAction(tr("Hide Retime Controls"), this,
        [tc, clipId] { tc->setRetimeControlsShown(clipId, false); });
    menu->popup(viewport()->mapToGlobal(pos));
}

void TimelineWidget::showSpeedPointMenu(quint64 clipId, int64_t localFrame, QPoint pos)
{
    const ClipData* clip = editops::clipById(m_ctx.project->timeline()->data(), clipId);
    const AnimatedValue* c = clip ? retime::curve(*clip) : nullptr;
    if (!c || c->indexOf(localFrame) < 0)
        return;
    const Interp current = keyInterp(c->keys[c->indexOf(localFrame)]);
    TimelineController* tc = m_controller;
    if (m_retimeMenu)
        m_retimeMenu->close();
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("SpeedPointMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    m_retimeMenu = menu;
    auto* group = new QActionGroup(menu);
    const std::tuple<Interp, QString, const char*> ramps[] = {
        {Interp::Linear, tr("No Ramp (Linear)"), "RampLinear"},
        {Interp::Smooth, tr("Smooth Ramp"), "RampSmooth"},
        {Interp::EaseInOut, tr("Ease In and Out"), "RampEase"},
    };
    for (const auto& [interp, text, name] : ramps) {
        QAction* a = menu->addAction(text, this,
            [tc, clipId, localFrame, i = interp] { tc->setSpeedPointRamp(clipId, localFrame, i); });
        a->setCheckable(true);
        a->setChecked(current == interp);
        a->setObjectName(QLatin1String(name));
        group->addAction(a);
    }
    menu->addSeparator();
    QAction* del = menu->addAction(tr("Delete Speed Point"), this,
        [tc, clipId, localFrame] { tc->removeSpeedPoint(clipId, localFrame); });
    del->setObjectName(QStringLiteral("SpeedPointDelete"));
    menu->popup(viewport()->mapToGlobal(pos));
}

void TimelineWidget::showRetimeCurve(quint64 clipId)
{
    const ClipData* clip = editops::clipById(m_ctx.project->timeline()->data(), clipId);
    if (!clip)
        return;
    // A constant-speed clip gets its curve first (keys at both ends; no visible change), so
    // the curve editor has something to show and edit.
    if (!retime::hasCurve(*clip)) {
        m_ctx.project->beginEdit(tr("Retime Curve"));
        TimelineData& data = m_ctx.project->timeline()->mutableData();
        for (quint64 id : retimeops::partners(data, clipId)) {
            if (ClipData* c = editops::clipById(data, id))
                retimeops::ensureCurve(*c);
        }
        m_ctx.project->endEdit();
    }
    m_keyframeLanes->setCurveEditorOpen(clipId, true);
    if (CurveEditorWidget* w = m_keyframeLanes->curveWidget(clipId)) {
        const QString path = keyframeops::memberPath(QStringLiteral("retime"), keys::RetimeFrame);
        for (const QString& shown : w->shownPaths()) {
            if (shown != path)
                w->setCurveShown(shown, false);
        }
        w->setCurveShown(path, true);
    }
}

} // namespace kk
