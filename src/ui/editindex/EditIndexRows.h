// SPDX-License-Identifier: GPL-3.0-or-later
// EditIndexRows — the rows of the Edit Index (events and markers) computed from a timeline.
#pragma once

#include "core/TimelineData.h"

#include <QColor>
#include <QString>

#include <cstdint>
#include <vector>

namespace kk {
class MediaPool;
}

namespace kk::editindex {

/// One event (clip) of the Edit Index, like a line of an edit decision list.
struct EventRow
{
    int number = 0; ///< 1-based event number in record order
    quint64 clipId = 0;
    TrackRef track;
    QString trackName; ///< "V1", "A2", "ST1" or the user's track name
    QString reel; ///< source file name without extension (media), else the clip kind
    QString name; ///< clip name
    QString sourceIn; ///< source timecode of the first frame
    QString sourceOut; ///< source timecode after the last frame (exclusive, EDL style)
    int64_t recordIn = 0; ///< timeline frame of the first frame
    int64_t recordOut = 0; ///< first timeline frame after the clip
    QString notes; ///< transitions, speed, disabled / offline state (read-only)
    bool enabled = true;
};

/// One marker of the Edit Index: on the timeline ruler (clipId 0) or on a clip.
struct MarkerRow
{
    int number = 0; ///< 1-based, in timeline order
    quint64 clipId = 0; ///< 0 = timeline marker
    int64_t frame = 0; ///< timeline frame
    int64_t storedFrame = 0; ///< Marker::frame (clip-local for clip markers)
    QString name;
    QColor color;
    QString note;
    QString location; ///< "Timeline" or the clip's name
    bool locked = false; ///< the clip's track is locked (name and notes are read-only)
    int64_t duration = 1; ///< Marker::duration (frames; > 1 = a ranged marker)
};

/// Every clip of @p data as an event, ordered by record in, then video, audio, subtitle
/// tracks from the lowest. @p pool (may be null) supplies reel names and source timecodes.
[[nodiscard]] std::vector<EventRow> events(const TimelineData& data, const MediaPool* pool);

/// Every timeline marker and every clip marker inside its clip's visible range, ordered by
/// timeline frame (timeline markers first on the same frame).
[[nodiscard]] std::vector<MarkerRow> markers(const TimelineData& data);

/// Finds marker @p row in @p data (timeline list or the clip's list); nullptr if it is gone.
[[nodiscard]] Marker* findMarker(TimelineData& data, quint64 clipId, int64_t storedFrame);

} // namespace kk::editindex
