// SPDX-License-Identifier: GPL-3.0-or-later
// Timeline — QObject owning the TimelineData of the project plus the playhead.
#pragma once

#include "core/TimelineData.h"

#include <QObject>

#include <cstdint>
#include <memory>

namespace kk {

/// Owns the editable TimelineData and the playhead position.
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
    /// Creates an empty timeline (no tracks, 25 fps, 1920x1080).
    explicit Timeline(QObject* parent = nullptr);

    /// Current timeline state (read-only).
    [[nodiscard]] const TimelineData& data() const { return m_data; }
    /// Mutable access; invalidates the snapshot and marks a change as pending. Call
    /// notifyChanged() when done (Project::endEdit() does it for you).
    [[nodiscard]] TimelineData& mutableData();
    /// Replaces the whole state (load, undo/redo) and emits changed().
    void setData(TimelineData data);
    /// Immutable copy of the current state for render/playback threads. Cached until the next
    /// change, so repeated calls are cheap.
    [[nodiscard]] std::shared_ptr<const TimelineData> snapshot() const;
    /// Emits changed() if mutableData() was used since the last notification.
    void notifyChanged();
    /// Monotonic counter incremented on every change (useful for cheap "has it changed" checks).
    [[nodiscard]] quint64 revision() const { return m_revision; }

    /// Playhead position in timeline frames.
    [[nodiscard]] int64_t playhead() const { return m_playhead; }
    /// Moves the playhead (clamped to >= 0); emits playheadChanged() if it moved.
    void setPlayhead(int64_t frame);

signals:
    /// The timeline data changed (any edit, undo, redo or load).
    void changed();
    /// The playhead moved to @p frame.
    void playheadChanged(int64_t frame);

private:
    TimelineData m_data;
    mutable std::shared_ptr<const TimelineData> m_snapshot;
    bool m_pendingChange = false;
    quint64 m_revision = 0;
    int64_t m_playhead = 0;
};

} // namespace kk
