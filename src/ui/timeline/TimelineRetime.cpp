// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, part 6: retiming — Change Clip Speed, retime controls (Ctrl+R), freeze
// frame (Shift+R), the speed bar commands and the retime curve request (spec section 29).
#include "core/Project.h"
#include "core/Retime.h"
#include "core/RetimeOps.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <cmath>

namespace kk {

namespace ops = editops;
namespace rops = retimeops;
using timelineedit::runEdit;

namespace {
/// Length of a freeze segment inserted from the speed menu.
constexpr double kFreezeSegmentSeconds = 1.0;
} // namespace

QVector<quint64> TimelineController::retimeTargets() const
{
    QVector<quint64> ids = effectiveSelection();
    if (!ids.isEmpty())
        return ids;
    // Nothing selected: the clips under the playhead on the auto-select video tracks.
    const TimelineData& data = timeline()->data();
    const int64_t playhead = timeline()->playhead();
    for (const TrackData& t : data.videoTracks) {
        if (!t.autoSelect)
            continue;
        if (const ClipData* c = t.clipAt(playhead))
            ids.append(c->id);
    }
    return withLinks(ids);
}

bool TimelineController::changeSelectionSpeed(const rops::SpeedChange& change)
{
    const QVector<quint64> ids = rops::primaries(timeline()->data(), retimeTargets());
    if (ids.isEmpty())
        return false;
    rops::SpeedChange c = change;
    if (c.freeze && c.freezeFrame < 0)
        c.freezeFrame = timeline()->playhead();
    return runEdit(project(), tr("Change Clip Speed"), [&](TimelineData& data) {
        bool any = false;
        for (quint64 id : ids)
            any |= rops::changeClipSpeed(data, id, c);
        return any;
    });
}

void TimelineController::toggleRetimeControls()
{
    const QVector<quint64> ids = retimeTargets();
    if (ids.isEmpty())
        return;
    // Like Resolve: one toggle for the whole selection, decided by its first clip.
    const bool show = !m_retimeShown.contains(ids.front());
    for (quint64 id : ids) {
        if (show)
            m_retimeShown.insert(id);
        else
            m_retimeShown.remove(id);
    }
    emit retimeControlsChanged();
}

void TimelineController::clearRetimeControls()
{
    if (m_retimeShown.isEmpty())
        return;
    m_retimeShown.clear();
    emit retimeControlsChanged();
}

bool TimelineController::retimeControlsShown(quint64 clipId) const
{
    return m_retimeShown.contains(clipId);
}

void TimelineController::setRetimeControlsShown(quint64 clipId, bool shown)
{
    bool changed = false;
    for (quint64 id : rops::partners(timeline()->data(), clipId)) {
        if (shown && !m_retimeShown.contains(id)) {
            m_retimeShown.insert(id);
            changed = true;
        } else if (!shown && m_retimeShown.remove(id)) {
            changed = true;
        }
    }
    if (changed)
        emit retimeControlsChanged();
}

bool TimelineController::freezeFrame()
{
    const QVector<quint64> ids = rops::primaries(timeline()->data(), retimeTargets());
    if (ids.isEmpty())
        return false;
    const int64_t playhead = timeline()->playhead();
    return runEdit(project(), tr("Freeze Frame"), [&](TimelineData& data) {
        bool any = false;
        for (quint64 id : ids)
            any |= rops::freezeClip(data, id, playhead);
        return any;
    });
}

void TimelineController::showRetimeCurve()
{
    const QVector<quint64> ids = retimeTargets();
    if (!ids.isEmpty())
        emit retimeCurveRequested(ids.front());
}

bool TimelineController::addSpeedPointAtPlayhead(quint64 clipId)
{
    const int64_t playhead = timeline()->playhead();
    return runEdit(project(), tr("Add Speed Point"),
        [&](TimelineData& data) { return rops::addSpeedPoint(data, clipId, playhead); });
}

bool TimelineController::removeSpeedPoint(quint64 clipId, int64_t localFrame)
{
    return runEdit(project(), tr("Delete Speed Point"),
        [&](TimelineData& data) { return rops::removeSpeedPoint(data, clipId, localFrame); });
}

bool TimelineController::setSegmentSpeed(quint64 clipId, int segment, double speed)
{
    return runEdit(project(), tr("Change Speed"),
        [&](TimelineData& data) { return rops::setSegmentSpeed(data, clipId, segment, speed); });
}

bool TimelineController::reverseSegment(quint64 clipId, int segment)
{
    return runEdit(project(), tr("Reverse Segment"),
        [&](TimelineData& data) { return rops::reverseSegment(data, clipId, segment); });
}

bool TimelineController::freezeSegmentAtPlayhead(quint64 clipId)
{
    const TimelineData& d = timeline()->data();
    const int64_t frames
        = std::max<int64_t>(1, std::llround(kFreezeSegmentSeconds * d.fps.toDouble()));
    const int64_t playhead = timeline()->playhead();
    return runEdit(project(), tr("Freeze Frame"),
        [&](TimelineData& data) { return rops::insertFreeze(data, clipId, playhead, frames); });
}

bool TimelineController::resetClipSpeed(quint64 clipId)
{
    return runEdit(project(), tr("Reset Speed"),
        [&](TimelineData& data) { return rops::resetClip(data, clipId); });
}

bool TimelineController::setSpeedPointRamp(quint64 clipId, int64_t localFrame, Interp ramp)
{
    return runEdit(project(), tr("Speed Ramp"),
        [&](TimelineData& data) { return rops::setPointRamp(data, clipId, localFrame, ramp); });
}

bool TimelineController::setRetimeProcess(quint64 clipId, RetimeProcess process)
{
    return runEdit(project(), tr("Retime Process"),
        [&](TimelineData& data) { return rops::setProcess(data, clipId, process); });
}

bool TimelineController::setPitchCorrection(quint64 clipId, bool on)
{
    return runEdit(project(), tr("Pitch Correction"),
        [&](TimelineData& data) { return rops::setPitchCorrection(data, clipId, on); });
}

} // namespace kk
