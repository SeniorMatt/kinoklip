// SPDX-License-Identifier: GPL-3.0-or-later
// Timeline QObject: change notification, snapshot caches and playhead.
#include "core/Timeline.h"

#include <algorithm>

namespace kk {

Timeline::Timeline(QObject* parent)
    : QObject(parent)
{ }

void Timeline::invalidate()
{
    m_plain.reset();
    m_snapshot.reset();
}

TimelineData& Timeline::mutableData()
{
    invalidate();
    m_pendingChange = true;
    return m_data;
}

void Timeline::setData(TimelineData data)
{
    m_data = std::move(data);
    m_data.nested = {};
    invalidate();
    m_pendingChange = true;
    notifyChanged();
}

void Timeline::setSharedData(std::shared_ptr<const TimelineData> data)
{
    m_data = *data;
    m_data.nested = {};
    invalidate();
    if (!data->nested.map)
        m_plain = std::move(data);
    m_pendingChange = true;
    notifyChanged();
}

std::shared_ptr<const TimelineData> Timeline::plainSnapshot() const
{
    if (!m_plain)
        m_plain = std::make_shared<const TimelineData>(m_data);
    return m_plain;
}

std::shared_ptr<const TimelineData> Timeline::snapshot() const
{
    if (m_snapshot)
        return m_snapshot;
    std::shared_ptr<const TimelineData> plain = plainSnapshot();
    NestedTimelines nested;
    if (m_nestedResolver)
        nested = m_nestedResolver(m_data);
    if (!nested.map || nested.map->isEmpty()) {
        m_snapshot = std::move(plain); // the common case: no nested clips, no second copy
    } else {
        auto copy = std::make_shared<TimelineData>(*plain);
        copy->nested = std::move(nested);
        m_snapshot = std::move(copy);
    }
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

void Timeline::setNestedResolver(NestedResolver resolver)
{
    m_nestedResolver = std::move(resolver);
    m_snapshot.reset();
}

void Timeline::nestedContentChanged()
{
    m_snapshot.reset();
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

} // namespace kk
