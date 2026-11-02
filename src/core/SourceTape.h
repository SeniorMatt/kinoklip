// SPDX-License-Identifier: GPL-3.0-or-later
// SourceTape — the Cut page's "source tape": every clip of a bin played back to back as one long
// tape, and the mapping between tape frames and (clip, source time).
#pragma once

#include "core/MediaItem.h"
#include "core/Rational.h"
#include "core/TimelineData.h"

#include <QString>
#include <QVector>

#include <cstdint>
#include <vector>

namespace kk {

/// One clip on the tape.
struct SourceTapeEntry
{
    QString mediaId;
    QString name;
    int64_t start = 0; ///< first tape frame
    int64_t duration = 0; ///< tape frames (>= 1)
    int64_t sourceDurationUs = 0; ///< media length in µs; 0 = still image
    bool hasVideo = false;
    bool hasAudio = false;
    ClipAttributes attributes; ///< the item's Clip Attributes (rotation, pixel aspect, ...)
    [[nodiscard]] int64_t end() const { return start + duration; }
};

/// Part of a media item covered by a tape range, in source microseconds.
struct SourceTapeSegment
{
    QString mediaId;
    int64_t inUs = 0;
    int64_t outUs = 0; ///< exclusive, > inUs (stills: 0 / 0, the edit uses the still length)
    friend bool operator==(const SourceTapeSegment&, const SourceTapeSegment&) = default;
};

/// A tape position resolved to its clip.
struct SourceTapePosition
{
    int entry = -1; ///< index into SourceTape::entries(), -1 = outside the tape
    int64_t sourceUs = 0; ///< source time of the media at that frame
};

/// The clips of a bin laid out back to back at one frame rate (the timeline's), with no gaps,
/// in the order given (the media pool's view order). Value type, pure logic: no Qt objects, so
/// the mapping is unit-tested (tests/core/test_sourcetape.cpp).
///
/// Tape frame f of entry e shows source time framesToUs(f - e.start, fps) of e's media. Each
/// clip occupies its whole media length rounded up to whole frames, stills a fixed length.
///
/// Thread-safety: plain value type.
class SourceTape
{
public:
    SourceTape() = default;

    /// Builds the tape for @p items in that order at @p fps. Items without picture or sound
    /// (timelines, compound and multicam clips, subtitles, offline or unprobed files) are
    /// skipped; stills get @p stillFrames frames.
    [[nodiscard]] static SourceTape build(
        const QVector<const MediaItem*>& items, Rational fps, int64_t stillFrames);

    [[nodiscard]] Rational fps() const { return m_fps; }
    [[nodiscard]] const std::vector<SourceTapeEntry>& entries() const { return m_entries; }
    [[nodiscard]] bool isEmpty() const { return m_entries.empty(); }
    /// Total length in tape frames.
    [[nodiscard]] int64_t duration() const;

    /// Index of the entry containing tape frame @p frame, -1 outside the tape (binary search).
    [[nodiscard]] int entryAt(int64_t frame) const;
    /// Index of the first entry showing media @p mediaId, or -1.
    [[nodiscard]] int indexOf(const QString& mediaId) const;
    /// Entry and source time of tape frame @p frame.
    [[nodiscard]] SourceTapePosition locate(int64_t frame) const;
    /// Tape frame showing source time @p sourceUs of entry @p entry (clamped into the entry),
    /// or -1 for an invalid index.
    [[nodiscard]] int64_t frameOf(int entry, int64_t sourceUs) const;
    /// Clip boundaries: every entry start plus the tape end, ascending.
    [[nodiscard]] std::vector<int64_t> boundaries() const;

    /// The media covered by tape frames [@p in, @p out) (clamped to the tape), split at the clip
    /// boundaries: one segment per clip touched, in tape order. Empty for an empty range.
    [[nodiscard]] QVector<SourceTapeSegment> segments(int64_t in, int64_t out) const;

    /// A playable private timeline of the tape (V1 / A1, one linked clip pair per entry) named
    /// @p name, @p width x @p height, at the tape's frame rate.
    [[nodiscard]] TimelineData toTimeline(const QString& name, int width, int height) const;

private:
    Rational m_fps;
    std::vector<SourceTapeEntry> m_entries;
};

} // namespace kk
