// SPDX-License-Identifier: GPL-3.0-or-later
// Timeline QObject: change notification, snapshot cache and playhead.
#include "core/Timeline.h"

#include <algorithm>

namespace cw {

Timeline::Timeline(QObject* parent)
    : QObject(parent)
{ }

TimelineData& Timeline::mutableData()
{
    m_snapshot.reset();
    m_pendingChange = true;
    return m_data;
}

void Timeline::setData(TimelineData data)
{
    m_data = std::move(data);
    m_snapshot.reset();
    m_pendingChange = true;
    notifyChanged();
}

std::shared_ptr<const TimelineData> Timeline::snapshot() const
{
    if (!m_snapshot)
        m_snapshot = std::make_shared<const TimelineData>(m_data);
    return m_snapshot;
}

void Timeline::notifyChanged()
{
    if (!m_pendingChange)
        return;
    m_pendingChange = false;
    ++m_revision;
    emit changed();
}

void Timeline::setPlayhead(int64_t frame)
{
    frame = std::max<int64_t>(0, frame);
    if (frame == m_playhead)
        return;
    m_playhead = frame;
    emit playheadChanged(frame);
}

} // namespace cw
