// SPDX-License-Identifier: GPL-3.0-or-later
// Project: the Color page's undoable entry points (clip grades, the timeline grade).
#include "core/EditOps.h"
#include "core/Project.h"

namespace kk {

namespace {

/// The grade slot of clip @p clipId in @p data, or nullptr (unknown clip, locked track).
ClipGrade* mutableClipGrade(TimelineData& data, quint64 clipId)
{
    const auto loc = editops::findClip(data, clipId);
    TrackData* track = loc ? data.track(loc->track) : nullptr;
    if (!track || track->locked)
        return nullptr;
    return &track->clips[size_t(loc->clipIndex)].grade;
}

} // namespace

bool Project::setClipGrade(
    quint64 timelineUid, quint64 clipId, const ClipGrade& grade, const QString& label)
{
    return setClipGrades(timelineUid, {clipId}, grade, label) == 1
        || clipGrade(timelineUid, clipId) == grade;
}

int Project::setClipGrades(quint64 timelineUid, const QVector<quint64>& clipIds,
    const ClipGrade& grade, const QString& label)
{
    const auto source = timelineData(timelineUid);
    if (!source)
        return 0;
    // Which clips change (unknown / locked / already equal ones are skipped).
    QVector<quint64> changing;
    for (const quint64 id : clipIds) {
        const auto loc = editops::findClip(*source, id);
        const TrackData* track = loc ? source->track(loc->track) : nullptr;
        if (!track || track->locked || changing.contains(id))
            continue;
        if (track->clips[size_t(loc->clipIndex)].grade != grade)
            changing.append(id);
    }
    if (changing.isEmpty())
        return 0;
    if (timelineUid == m_activeUid) {
        beginEdit(label);
        for (const quint64 id : std::as_const(changing)) {
            if (ClipGrade* slot = mutableClipGrade(m_timeline->mutableData(), id))
                *slot = grade;
        }
        endEdit();
        return int(changing.size());
    }
    auto copy = std::make_shared<TimelineData>(*source);
    for (const quint64 id : std::as_const(changing)) {
        if (ClipGrade* slot = mutableClipGrade(*copy, id))
            *slot = grade;
    }
    beginEdit(label);
    replaceTimeline(timelineUid, std::move(copy));
    endEdit();
    return int(changing.size());
}

ClipGrade Project::clipGrade(quint64 timelineUid, quint64 clipId) const
{
    const auto data = timelineData(timelineUid);
    const ClipData* clip = data ? editops::clipById(*data, clipId) : nullptr;
    return clip ? clip->grade : ClipGrade {};
}

bool Project::setTimelineGrade(quint64 timelineUid, const ClipGrade& grade, const QString& label)
{
    const auto source = timelineData(timelineUid);
    if (!source)
        return false;
    if (source->grade == grade)
        return true; // nothing to do (and no empty undo step)
    if (timelineUid == m_activeUid) {
        beginEdit(label);
        m_timeline->mutableData().grade = grade;
        endEdit();
        return true;
    }
    auto copy = std::make_shared<TimelineData>(*source);
    copy->grade = grade;
    beginEdit(label);
    replaceTimeline(timelineUid, std::move(copy));
    endEdit();
    return true;
}

ClipGrade Project::timelineGrade(quint64 timelineUid) const
{
    const auto data = timelineData(timelineUid);
    return data ? data->grade : ClipGrade {};
}

} // namespace kk
