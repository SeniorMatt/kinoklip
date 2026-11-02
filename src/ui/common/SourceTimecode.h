// SPDX-License-Identifier: GPL-3.0-or-later
// SourceTimecode — the source media timecode of a timeline clip (Edit Index, clip labels).
#pragma once

#include "core/Rational.h"

#include <QString>

#include <cstdint>

namespace kk {

class MediaPool;
struct ClipData;

/// Timecode origin and rate of the media behind a clip.
struct SourceTimecodeBase
{
    Rational rate; ///< frame rate the source timecode counts in
    int64_t startUs = 0; ///< source time 0 shows this timecode (µs from 00:00:00:00)
};

/// Rate and start of the source timecode of @p clip: the media's frame rate (else
/// @p timelineFps for stills, audio, generated and nested clips) and its start timecode
/// (Clip Attributes > Timecode override first, then the file's embedded timecode, else 0).
/// @p pool may be null (no start timecode then).
[[nodiscard]] SourceTimecodeBase sourceTimecodeBase(
    const ClipData& clip, const MediaPool* pool, Rational timelineFps);

/// "HH:MM:SS:FF" of source time @p sourceUs of @p clip (see sourceTimecodeBase()).
[[nodiscard]] QString sourceTimecode(
    const ClipData& clip, int64_t sourceUs, const MediaPool* pool, Rational timelineFps);

} // namespace kk
