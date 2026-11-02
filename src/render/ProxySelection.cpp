// SPDX-License-Identifier: GPL-3.0-or-later
// ProxySelection — original / proxy / optimized media choice per media pool item.
#include "render/ProxySelection.h"

#include "core/MediaItem.h"

#include <QFileInfo>
#include <QSet>

namespace kk {

namespace {

constexpr int kMaxNestingDepth = 16;

bool usesIn(const TimelineData& tl, const NestedTimelines& nested, const ProxyMap& proxies,
    int depth, QSet<quint64>& seen)
{
    for (const TrackData& t : tl.videoTracks) {
        for (const ClipData& c : t.clips) {
            if (c.kind == ClipKind::Media && proxies.contains(c.mediaId))
                return true;
            if (c.kind == ClipKind::Nested && depth < kMaxNestingDepth
                && !seen.contains(c.nestedUid)) {
                seen.insert(c.nestedUid);
                if (const TimelineData* n = nested.find(c.nestedUid);
                    n && usesIn(*n, nested, proxies, depth + 1, seen))
                    return true;
            }
        }
    }
    return false;
}

} // namespace

std::shared_ptr<const ProxyMap> makeProxyMap(
    const MediaPoolState& pool, const MediaLinks& links, ProxyHandling handling, bool useOptimized)
{
    auto map = std::make_shared<ProxyMap>();
    for (const MediaItem& item : pool.items) {
        if (item.isTimeline() || !item.info.hasVideo || item.info.isStillImage)
            continue;
        const bool proxyReady = handling != ProxyHandling::Disabled
            && links.state(item.id, LinkKind::Proxy, item.path) == LinkState::Ready;
        const bool optimizedReady = useOptimized
            && links.state(item.id, LinkKind::Optimized, item.path) == LinkState::Ready;
        bool useProxy = false;
        if (handling == ProxyHandling::PreferProxies)
            useProxy = proxyReady;
        else if (handling == ProxyHandling::PreferOriginals)
            useProxy = proxyReady && !optimizedReady && !QFileInfo::exists(item.path);
        const LinkKind kind = useProxy ? LinkKind::Proxy : LinkKind::Optimized;
        if (!useProxy && !optimizedReady)
            continue;
        ProxyRef ref;
        ref.path = links.link(item.id, kind).path;
        ref.displaySize = QSizeF(item.info.displayWidth, item.info.displayHeight);
        if (ref.displaySize.isEmpty())
            ref.displaySize = QSizeF(item.info.width, item.info.height);
        if (ref.displaySize.isEmpty())
            continue; // unknown geometry: keep the original
        map->insert(item.id, ref);
    }
    return map;
}

std::shared_ptr<const MediaPathMap> makeAudioPathMap(
    const MediaPoolState& pool, const MediaLinks& links, ProxyHandling handling)
{
    auto map = std::make_shared<MediaPathMap>();
    map->reserve(pool.items.size());
    for (const MediaItem& item : pool.items) {
        QString path = item.path;
        if (handling != ProxyHandling::Disabled && !item.isTimeline() && item.info.hasAudio
            && !path.isEmpty() && !QFileInfo::exists(path)
            && links.state(item.id, LinkKind::Proxy, item.path) == LinkState::Ready)
            path = links.link(item.id, LinkKind::Proxy).path;
        map->insert(item.id, path);
    }
    return map;
}

bool usesSubstitutes(const TimelineData& timeline, const ProxyMap& proxies)
{
    if (proxies.isEmpty())
        return false;
    QSet<quint64> seen;
    return usesIn(timeline, timeline.nested, proxies, 0, seen);
}

} // namespace kk
