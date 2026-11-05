// SPDX-License-Identifier: GPL-3.0-or-later
// DecodeTargets — which decoders a timeline frame needs, without touching any decoder: the
// look-ahead's view of the timeline (RenderRequest::prepare).
#pragma once

#include "core/TimelineData.h"
#include "render/RenderTypes.h"

#include <QString>

#include <cstdint>
#include <vector>

namespace kk::render {

/// One decoder position a frame needs.
struct DecodeTarget
{
    const ClipData* clip = nullptr;
    QString path; ///< the proxy's when the clip has one, as the renderer decodes it
    int64_t timeUs = 0;
};

/// Appends the media positions shown at timeline @p frame of @p tl: enabled video tracks, both
/// clips of a transition, the clips inside nested timelines, the next source frame of a blended
/// retime. Media a composition's nodes add and frames a time effect adds are left out: they
/// simply decode when they are needed. O(tracks log clips), no decoding.
void collectDecodeTargets(const TimelineData& tl, int64_t frame, const NestedTimelines& nested,
    const MediaPathMap* media, const ProxyMap* proxies, std::vector<DecodeTarget>& out);

/// True if a decoder that served @p before reaches @p target with no real work: the same clip
/// playing on, or a position in the same file a few frames away (a razor cut, a jump cut of a
/// few frames), which decoding forward covers within a frame's time. Nothing to prepare then.
[[nodiscard]] bool continuesFrom(
    const DecodeTarget& target, const std::vector<DecodeTarget>& before);

} // namespace kk::render
