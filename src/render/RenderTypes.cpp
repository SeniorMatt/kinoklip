// SPDX-License-Identifier: GPL-3.0-or-later
// RenderTypes — helpers for the shared render value types.
#include "render/RenderTypes.h"

#include "core/MediaItem.h"

#include <algorithm>
#include <cmath>

namespace kk {

QSize scaledRenderSize(QSize requested, Quality quality)
{
    const double f = qualityFactor(quality);
    return QSize(std::max(1, int(std::lround(requested.width() * f))),
        std::max(1, int(std::lround(requested.height() * f))));
}

std::shared_ptr<const MediaPathMap> makeMediaPathMap(const MediaPoolState& pool)
{
    auto map = std::make_shared<MediaPathMap>();
    map->reserve(pool.items.size());
    for (const MediaItem& item : pool.items)
        map->insert(item.id, item.path);
    return map;
}

} // namespace kk
