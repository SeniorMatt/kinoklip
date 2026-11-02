// SPDX-License-Identifier: GPL-3.0-or-later
// ClipGeometry implementation (media pool lookup + render::fitSize()).
#include "ui/common/ClipGeometry.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "render/TimelineEval.h"

namespace kk {

QSizeF clipContentSize(const Project* project, const ClipData& clip, QSizeF frameSize)
{
    if (!project)
        return frameSize;
    if (clip.kind == ClipKind::Nested) {
        const auto nested = project->timelineData(clip.nestedUid);
        if (!nested || nested->width <= 0 || nested->height <= 0)
            return frameSize;
        return render::fitSize(QSizeF(nested->width, nested->height), frameSize);
    }
    if (clip.kind != ClipKind::Media)
        return frameSize;
    const MediaItem* item = project->mediaPool()->item(clip.mediaId);
    if (!item || item->info.displayWidth <= 0 || item->info.displayHeight <= 0)
        return frameSize;
    return render::fitSize(QSizeF(item->info.displayWidth, item->info.displayHeight), frameSize);
}

} // namespace kk
