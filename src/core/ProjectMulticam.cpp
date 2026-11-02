// SPDX-License-Identifier: GPL-3.0-or-later
// Project, part 3: multicam clips (create from synced sources, flatten).
#include "core/EditOps.h"
#include "core/Multicam.h"
#include "core/Project.h"

namespace kk {

quint64 Project::createMulticamClip(const QVector<multicam::AngleSource>& sources,
    const QString& name, Rational fps, int width, int height, const QString& binId,
    bool moveSources, QString* error)
{
    const quint64 uid = m_nextTimelineUid;
    const QString finalName = name.isEmpty() ? uniqueTimelineName(tr("Multicam Clip")) : name;
    std::optional<TimelineData> mc
        = multicam::build(sources, uid, finalName, fps, width, height, error);
    if (!mc)
        return 0;
    ++m_nextTimelineUid;
    mc->retimeProcess = m_format.retimeProcess;
    // Follows the project format when it matches it (like a new timeline), else keeps its own.
    mc->useProjectSettings
        = fps == m_format.fps && mc->width == m_format.width && mc->height == m_format.height;
    beginEdit(tr("New Multicam Clip"));
    addTimeline(std::move(*mc), binId);
    if (moveSources) {
        // Resolve's option: the source clips go into a bin of their own next to the new clip.
        const QString bin = m_mediaPool->addBin(tr("Original Clips"), binId);
        for (const multicam::AngleSource& s : sources)
            m_mediaPool->moveItemToBin(s.mediaId, bin);
    }
    endEdit();
    return uid;
}

std::optional<QVector<quint64>> Project::flattenMulticamClip(quint64 clipId, QString* error)
{
    TimelineData copy = m_timeline->data();
    std::optional<QVector<quint64>> restored
        = multicam::flatten(copy, {clipId}, timelineLookup(), error);
    if (!restored)
        return std::nullopt;
    beginEdit(tr("Flatten Multicam Clip"));
    m_timeline->mutableData() = std::move(copy);
    endEdit();
    return restored;
}

} // namespace kk
