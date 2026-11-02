// SPDX-License-Identifier: GPL-3.0-or-later
// Media usage: which media pool items the project's timelines use (Usage column, smart bin
// "Usage" rule, Remove Unused Clips, Select Clips in Timeline).
#pragma once

#include "core/MediaItem.h"
#include "core/TimelineData.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

#include <memory>
#include <vector>

namespace kk {

class Project;

namespace usage {

/// Media pool item id -> number of uses. An item without uses is not listed.
using Counts = QHash<QString, int>;

/// Counts the uses of every media pool item in @p timelines (every timeline of the project,
/// compound clip timelines included, so media used only inside a compound clip or a nested
/// timeline counts as used). Media clips count for their MediaItem::id, Nested clips for the
/// item of the timeline they show (@p pool maps timeline uids to items). A linked video +
/// audio pair is one use. O(clips).
[[nodiscard]] Counts compute(
    const std::vector<std::shared_ptr<const TimelineData>>& timelines, const MediaPoolState& pool);
/// compute() over every timeline of @p project.
[[nodiscard]] Counts compute(const Project& project);

/// Ids of the clips of @p data that use one of the media pool items @p itemIds (Media clips by
/// mediaId, Nested clips by the item of the timeline they show), in track order.
[[nodiscard]] QVector<quint64> clipsUsing(
    const TimelineData& data, const QSet<QString>& itemIds, const MediaPoolState& pool);

/// Media pool item id a timeline clip refers to: the media item of a Media clip, the timeline
/// item of a Nested clip; empty for titles, solids and offline references.
[[nodiscard]] QString itemIdOf(const ClipData& clip, const MediaPoolState& pool);

/// Media file items (MediaItemKind::File) of @p pool without any use in @p counts.
[[nodiscard]] QStringList unusedFileItems(const MediaPoolState& pool, const Counts& counts);

} // namespace usage
} // namespace kk
