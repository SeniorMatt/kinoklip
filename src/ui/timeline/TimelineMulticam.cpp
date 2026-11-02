// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, multicam part: cut / switch angles at the playhead (multicam viewer
// clicks, number keys), Switch Multicam Clip Angle, Flatten Multicam Clip.
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"

#include <QAction>

namespace kk {

namespace {

bool checked(const AppContext& ctx, const char* id)
{
    return ctx.actions && ctx.actions->contains(QLatin1String(id))
        && ctx.actions->action(QLatin1String(id))->isChecked();
}

} // namespace

bool TimelineController::multicamSwitchMode() const
{
    return checked(m_ctx, "multicam.mode_switch");
}

multicam::AngleTarget TimelineController::multicamTarget() const
{
    if (checked(m_ctx, "multicam.target_video"))
        return multicam::AngleTarget::Video;
    if (checked(m_ctx, "multicam.target_audio"))
        return multicam::AngleTarget::Audio;
    return multicam::AngleTarget::Both;
}

bool TimelineController::chooseMulticamAngle(int angle)
{
    const multicam::AngleTarget target = multicamTarget();
    if (!multicamSwitchMode())
        return cutMulticamAngle(angle, target);
    const quint64 clip = multicam::multicamClipAt(timeline()->data(), timeline()->playhead(),
        project()->timelineLookup(), target == multicam::AngleTarget::Audio);
    return clip && switchMulticamAngle(clip, angle, target);
}

bool TimelineController::cutMulticamAngle(int angle, multicam::AngleTarget target)
{
    Project* p = project();
    if (p->isEditing())
        return false;
    // The playhead the user sees: while playing, Timeline::playhead() is written back at up
    // to 30 Hz, so the cut lands within a frame or two of what is on screen (Resolve cuts
    // "live" the same way).
    const int64_t frame = timeline()->playhead();
    TimelineData copy = timeline()->data();
    const QVector<quint64> shown
        = multicam::cutToAngle(copy, frame, angle, target, p->timelineLookup());
    if (shown.isEmpty())
        return false;
    p->beginEdit(tr("Multicam Cut"));
    timeline()->mutableData() = std::move(copy);
    p->endEdit();
    emit multicamAngleChosen();
    return true;
}

bool TimelineController::switchMulticamAngle(
    quint64 clipId, int angle, multicam::AngleTarget target)
{
    Project* p = project();
    if (p->isEditing())
        return false;
    TimelineData copy = timeline()->data();
    if (!multicam::switchAngle(copy, clipId, angle, target, p->timelineLookup()))
        return false;
    p->beginEdit(tr("Switch Multicam Clip Angle"));
    timeline()->mutableData() = std::move(copy);
    p->endEdit();
    emit multicamAngleChosen();
    return true;
}

bool TimelineController::flattenMulticamSelection()
{
    Project* p = project();
    const auto lookup = p->timelineLookup();
    const TimelineData& data = timeline()->data();
    QVector<quint64> clips;
    QSet<quint64> covered;
    for (const quint64 id : effectiveSelection()) {
        const ClipData* c = editops::clipById(data, id);
        if (!c || covered.contains(id) || !multicam::isMulticamClip(*c, lookup))
            continue;
        clips.append(id);
        for (const quint64 linked : editops::linkedClips(data, id))
            covered.insert(linked);
    }
    if (clips.isEmpty()) {
        if (const quint64 under = multicam::multicamClipAt(data, timeline()->playhead(), lookup))
            clips.append(under);
    }
    if (clips.isEmpty()) {
        if (m_messageSink)
            m_messageSink(tr("Flatten Multicam Clip"), tr("Select a multicam clip first."));
        return false;
    }
    QVector<quint64> restored;
    for (const quint64 id : std::as_const(clips)) {
        QString error;
        const auto r = p->flattenMulticamClip(id, &error);
        if (!r) {
            if (m_messageSink)
                m_messageSink(tr("Flatten Multicam Clip"), error);
            continue;
        }
        restored += *r;
    }
    if (restored.isEmpty())
        return false;
    m_ctx.selection->setClips(restored);
    return true;
}

} // namespace kk
