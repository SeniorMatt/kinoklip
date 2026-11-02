// SPDX-License-Identifier: GPL-3.0-or-later
// ClipGeometry implementation (media pool lookup + render::fitSize()).
#include "ui/common/ClipGeometry.h"

#include "core/MediaPool.h"
#include "render/TimelineEval.h"

namespace kk {

QSizeF clipContentSize(const MediaPool* pool, const ClipData& clip, QSizeF frameSize)
{
    if (clip.kind != ClipKind::Media || !pool)
        return frameSize;
    const MediaItem* item = pool->item(clip.mediaId);
    if (!item || item->info.displayWidth <= 0 || item->info.displayHeight <= 0)
        return frameSize;
    return render::fitSize(QSizeF(item->info.displayWidth, item->info.displayHeight), frameSize);
}

} // namespace kk
