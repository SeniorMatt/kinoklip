// SPDX-License-Identifier: GPL-3.0-or-later
// DecodeTargets — the decoder positions of a timeline frame.
#include "render/DecodeTargets.h"

#include "core/Retime.h"
#include "render/TimelineEval.h"

#include <cstdlib>

namespace kk::render {

namespace {

/// Same file, this close: decoding forward is cheaper than keeping another decoder ready.
constexpr int64_t kContinuousUs = 100'000;
/// Same limit as the renderers (a timeline cannot contain itself; guards damaged data).
constexpr int kMaxDepth = 16;

void collectClip(const ClipData* clip, const TimelineData& tl, int64_t frame,
    const NestedTimelines& nested, const MediaPathMap* media, const ProxyMap* proxies, int depth,
    std::vector<DecodeTarget>& out);

void collectTimeline(const TimelineData& tl, int64_t frame, const NestedTimelines& nested,
    const MediaPathMap* media, const ProxyMap* proxies, int depth, std::vector<DecodeTarget>& out)
{
    for (const TrackData& track : tl.videoTracks) {
        if (!track.enabled)
            continue;
        const TrackFrame tf = evaluateTrack(track, frame);
        if (tf.single) {
            collectClip(tf.single, tl, frame, nested, media, proxies, depth, out);
        } else if (tf.transition) {
            collectClip(tf.transition->outgoing, tl, frame, nested, media, proxies, depth, out);
            collectClip(tf.transition->incoming, tl, frame, nested, media, proxies, depth, out);
        }
    }
}

void collectClip(const ClipData* clip, const TimelineData& tl, int64_t frame,
    const NestedTimelines& nested, const MediaPathMap* media, const ProxyMap* proxies, int depth,
    std::vector<DecodeTarget>& out)
{
    if (!clip || !clip->enabled)
        return;
    if (clip->kind == ClipKind::Nested) {
        const TimelineData* n = nested.findClip(*clip);
        if (n && depth < kMaxDepth) {
            collectTimeline(*n, nestedFrameAt(*clip, frame, tl.fps, n->fps), nested, media, proxies,
                depth + 1, out);
        }
        return;
    }
    if (clip->kind != ClipKind::Media)
        return;
    QString path;
    if (proxies) {
        const auto it = proxies->constFind(clip->mediaId);
        if (it != proxies->constEnd())
            path = it->path;
    }
    if (path.isEmpty() && media)
        path = media->value(clip->mediaId);
    if (path.isEmpty())
        return;
    out.push_back({clip, path, clampedSourceTimeUs(*clip, frame, tl.fps)});
    if (retime::effectiveProcess(*clip, tl) != RetimeProcess::Nearest)
        out.push_back({clip, path, clampedSourceTimeUs(*clip, frame + 1, tl.fps)});
}

} // namespace

void collectDecodeTargets(const TimelineData& tl, int64_t frame, const NestedTimelines& nested,
    const MediaPathMap* media, const ProxyMap* proxies, std::vector<DecodeTarget>& out)
{
    collectTimeline(tl, frame, nested, media, proxies, 0, out);
}

bool continuesFrom(const DecodeTarget& target, const std::vector<DecodeTarget>& before)
{
    for (const DecodeTarget& b : before) {
        if (b.clip == target.clip)
            return true;
        if (b.path == target.path && std::abs(target.timeUs - b.timeUs) <= kContinuousUs)
            return true;
    }
    return false;
}

} // namespace kk::render
