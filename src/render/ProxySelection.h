// SPDX-License-Identifier: GPL-3.0-or-later
// ProxySelection — decides, per media pool item, whether the viewers decode the camera
// original, its proxy or its optimized media (Playback > Proxy Handling, Use Optimized Media
// If Available), and which file the audio comes from.
#pragma once

#include "core/MediaLinks.h"
#include "render/RenderTypes.h"

#include <memory>

namespace kk {

struct MediaPoolState;

/// Video substitutes for @p pool under @p handling / @p useOptimized:
/// - Prefer Proxies: a Ready proxy, else (when allowed) Ready optimized media, else the original.
/// - Prefer Camera Originals: the original while it is online; an offline original falls back
///   to its proxy; Ready optimized media is used when allowed (it is full quality).
/// - Disable All Proxies: never a proxy; optimized media when allowed.
/// Stale and missing links are never used. Stills and timeline items never get substitutes.
/// Stats files (GUI thread; rebuild when the pool, the links or the modes change).
[[nodiscard]] std::shared_ptr<const ProxyMap> makeProxyMap(
    const MediaPoolState& pool, const MediaLinks& links, ProxyHandling handling, bool useOptimized);

/// Audio paths: the originals, except that an offline original whose proxy is Ready plays
/// the proxy's audio (proxies carry the source's sound as PCM) unless proxies are disabled.
[[nodiscard]] std::shared_ptr<const MediaPathMap> makeAudioPathMap(
    const MediaPoolState& pool, const MediaLinks& links, ProxyHandling handling);

/// True if @p timeline (or a timeline it nests) shows a media item that @p proxies replaces:
/// the viewers' "Proxy" indicator.
[[nodiscard]] bool usesSubstitutes(const TimelineData& timeline, const ProxyMap& proxies);

} // namespace kk
