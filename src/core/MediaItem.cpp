// SPDX-License-Identifier: GPL-3.0-or-later
// Equality helpers for media pool value types.
#include "core/MediaItem.h"

namespace kk {

bool sameMediaInfo(const media::MediaInfo& a, const media::MediaInfo& b)
{
    return a.valid == b.valid && a.error == b.error && a.path == b.path && a.hasVideo == b.hasVideo
        && a.hasAudio == b.hasAudio && a.isStillImage == b.isStillImage && a.width == b.width
        && a.height == b.height && a.rotation == b.rotation && a.frameRate == b.frameRate
        && a.durationUs == b.durationUs && a.audioChannels == b.audioChannels
        && a.audioSampleRate == b.audioSampleRate && a.videoCodec == b.videoCodec
        && a.audioCodec == b.audioCodec && a.container == b.container;
}

bool operator==(const MediaItem& a, const MediaItem& b)
{
    return a.id == b.id && a.path == b.path && a.name == b.name && a.binId == b.binId
        && a.kind == b.kind && a.timelineUid == b.timelineUid && sameMediaInfo(a.info, b.info);
}

} // namespace kk
