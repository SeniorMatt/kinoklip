// SPDX-License-Identifier: GPL-3.0-or-later
// SourceTimecode — source timecode of a clip from its media item.
#include "ui/common/SourceTimecode.h"

#include "core/MediaPool.h"
#include "core/Multicam.h"
#include "core/TimelineData.h"
#include "ui/common/TimecodeFormat.h"

namespace kk {

SourceTimecodeBase sourceTimecodeBase(
    const ClipData& clip, const MediaPool* pool, Rational timelineFps)
{
    SourceTimecodeBase base {timelineFps, 0};
    const MediaItem* item = (pool && clip.kind == ClipKind::Media && !clip.mediaId.isEmpty())
        ? pool->item(clip.mediaId)
        : nullptr;
    if (!item)
        return base;
    if (item->info.frameRate.isValid())
        base.rate = item->info.frameRate;
    if (item->attributes.overrideTimecode)
        base.startUs = framesToUs(item->attributes.startTimecodeFrames, base.rate);
    else
        base.startUs = multicam::timecodeToUs(item->info.startTimecode, base.rate).value_or(0);
    return base;
}

QString sourceTimecode(
    const ClipData& clip, int64_t sourceUs, const MediaPool* pool, Rational timelineFps)
{
    const SourceTimecodeBase base = sourceTimecodeBase(clip, pool, timelineFps);
    return timecode::format(usToFrames(base.startUs + sourceUs, base.rate), base.rate);
}

} // namespace kk
