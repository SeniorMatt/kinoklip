// SPDX-License-Identifier: GPL-3.0-or-later
// UndoSnapshots — compact storage of the timeline snapshots kept by the undo history:
// consecutive steps share every clip that did not change.
#pragma once

#include "core/TimelineData.h"

#include <array>
#include <memory>
#include <vector>

namespace kk::undosnap {

/// One timeline as the undo history stores it: the timeline without its clips (`shell`, the
/// tracks keep every other field) plus each track's clips as shared, immutable ClipData.
///
/// Why: the snapshot undo (docs/archive/plan.md, "Undo") used to keep one full TimelineData copy per step,
/// about 0.85 MB for a 1,500-clip timeline (docs/archive/polish-v3.md, round 4 stress pass), so a long
/// session grew by hundreds of MB. Packed against the previous step, a step only owns the
/// clips it changed (plus one pointer per clip), and the shell is shared too when only clips
/// changed.
///
/// Thread-safety: immutable once built; unpack() caches its result in a mutable weak pointer,
/// so use it from one thread (the GUI thread, like the undo stack).
struct PackedTimeline
{
    /// The timeline with every track's `clips` empty.
    std::shared_ptr<const TimelineData> shell;
    /// clips[trackTypeIndex(type)][trackIndex] = the clips of that track, in order.
    std::array<std::vector<std::vector<std::shared_ptr<const ClipData>>>, kTrackTypeCount> clips;
    /// The full data this was packed from or last unpacked to, while something else keeps it
    /// alive (the live timeline, another snapshot): unpack() returns it without copying.
    mutable std::weak_ptr<const TimelineData> full;
};

/// Packs @p data. Clips (matched by id within the track of the same id) and the shell that
/// equal those of @p reference (the neighbouring step of the same timeline, may be null) are
/// shared with it instead of copied; if @p reference was packed from @p data itself it is
/// returned as is.
[[nodiscard]] std::shared_ptr<const PackedTimeline> pack(
    const std::shared_ptr<const TimelineData>& data,
    const std::shared_ptr<const PackedTimeline>& reference);

/// The full timeline of @p packed: the cached full data when it is still alive, else a new
/// copy (one TimelineData copy; it becomes the cached one).
[[nodiscard]] std::shared_ptr<const TimelineData> unpack(const PackedTimeline& packed);

/// Bytes owned by @p packed alone, i.e. not shared with @p other (clip and shell storage,
/// approximate: sizeof of the structs, not their heap strings). For tests and the benchmark.
[[nodiscard]] size_t exclusiveBytes(const PackedTimeline& packed, const PackedTimeline* other);

} // namespace kk::undosnap
