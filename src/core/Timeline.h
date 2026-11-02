// SPDX-License-Identifier: GPL-3.0-or-later
// Timeline — QObject owning the TimelineData of the active (edited) timeline plus the playhead.
#pragma once

#include "core/TimelineData.h"

#include <QObject>

#include <cstdint>
#include <functional>
#include <memory>

namespace kk {

/// Owns the editable TimelineData of the project's active timeline and the playhead.
///
/// A project has several timelines (Project::timelineUids()); this object always holds the
/// one being edited (the active tab). Switching tabs swaps its data and playhead and emits
/// changed() + playheadChanged(), so views connected once keep working.
///
/// Editing protocol (see core/readme.md): wrap changes in Project::beginEdit()/endEdit(), mutate
/// through mutableData() (usually via editops::*), and call notifyChanged() for live updates
/// during a drag; endEdit() notifies automatically.
///
/// Thread-safety: GUI thread only. Hand snapshot() to other threads — it is immutable.
class Timeline : public QObject
{
    Q_OBJECT
public:
    /// Supplies the nested-timeline map of a render snapshot (installed by the Project).
    using NestedResolver = std::function<NestedTimelines(const TimelineData&)>;

    /// Creates an empty timeline (no tracks, 25 fps, 1920x1080).
    explicit Timeline(QObject* parent = nullptr);

    /// Current timeline state (read-only).
    [[nodiscard]] const TimelineData& data() const { return m_data; }
    /// Mutable access; invalidates the snapshot and marks a change as pending. Call
    /// notifyChanged() when done (Project::endEdit() does it for you).
    [[nodiscard]] TimelineData& mutableData();
    /// Replaces the whole state (load, undo/redo) and emits changed().
    void setData(TimelineData data);
    /// Replaces the whole state with @p data (shared, becomes plainSnapshot()) and emits
    /// changed(). Cheaper than setData() when the caller already holds an immutable copy.
    void setSharedData(std::shared_ptr<const TimelineData> data);
    /// Immutable copy of the current state for render/playback threads, with
    /// TimelineData::nested filled (every timeline its Nested clips reach). Cached until the
    /// next change, so repeated calls are cheap.
    [[nodiscard]] std::shared_ptr<const TimelineData> snapshot() const;
    /// Immutable copy of the current state WITHOUT the nested map (undo history, project
    /// store). Cached until the next change.
    [[nodiscard]] std::shared_ptr<const TimelineData> plainSnapshot() const;
    /// Emits changed() if mutableData() was used since the last notification.
    void notifyChanged();
    /// Monotonic counter incremented on every change (useful for cheap "has it changed" checks).
    [[nodiscard]] quint64 revision() const { return m_revision; }

    /// Installs the resolver used by snapshot() (Project; empty = no nested content).
    void setNestedResolver(NestedResolver resolver);
    /// A nested timeline changed (outside this object): drops the cached render snapshot and
    /// emits changed() so views and viewers refresh.
    void nestedContentChanged();

    /// Playhead position in timeline frames.
    [[nodiscard]] int64_t playhead() const { return m_playhead; }
    /// Moves the playhead (clamped to >= 0); emits playheadChanged() if it moved.
    void setPlayhead(int64_t frame);

signals:
    /// The timeline data changed (any edit, undo, redo, load or tab switch).
    void changed();
    /// The playhead moved to @p frame.
    void playheadChanged(int64_t frame);

private:
    void invalidate();

    TimelineData m_data;
    mutable std::shared_ptr<const TimelineData> m_plain;
    mutable std::shared_ptr<const TimelineData> m_snapshot;
    NestedResolver m_nestedResolver;
    bool m_pendingChange = false;
    quint64 m_revision = 0;
    int64_t m_playhead = 0;
};

} // namespace kk
