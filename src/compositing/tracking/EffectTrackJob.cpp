// SPDX-License-Identifier: GPL-3.0-or-later
#include "compositing/tracking/EffectTrackJob.h"

#include "core/MatchMove.h"
#include "core/VideoEffectKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::tracking {

namespace {

namespace k = fxkeys;

constexpr int kTrackWidth = 640;

EffectInstance* findEffect(ClipData& clip, quint64 id)
{
    for (EffectInstance& fx : clip.effects) {
        if (fx.id == id)
            return &fx;
    }
    return nullptr;
}

} // namespace

const ClipData* findVideoClip(const TimelineData& tl, quint64 clipId, int* trackIndex)
{
    for (size_t t = 0; t < tl.videoTracks.size(); ++t) {
        for (const ClipData& c : tl.videoTracks[t].clips) {
            if (c.id == clipId) {
                if (trackIndex)
                    *trackIndex = int(t);
                return &c;
            }
        }
    }
    return nullptr;
}

std::optional<EffectTrackPlan> planEffectTrack(const TimelineData& tl, quint64 clipId,
    quint64 effectId, int64_t playhead, bool forward, QString* why)
{
    auto fail = [why](const QString& text) -> std::optional<EffectTrackPlan> {
        if (why)
            *why = text;
        return std::nullopt;
    };
    int trackIndex = 0;
    const ClipData* clip = findVideoClip(tl, clipId, &trackIndex);
    if (!clip)
        return fail(QStringLiteral("the clip is not on a video track"));
    const EffectInstance* fx = nullptr;
    for (const EffectInstance& e : clip->effects) {
        if (e.id == effectId)
            fx = &e;
    }
    if (!fx || fx->type != QLatin1String(fxid::MatchMove))
        return fail(QStringLiteral("the effect is missing"));
    if (!clip->contains(playhead))
        return fail(QStringLiteral("move the playhead into the clip first"));
    if (!tl.fps.isValid() || tl.width <= 0 || tl.height <= 0)
        return fail(QStringLiteral("the timeline has no frame size"));

    const ParamSet& p = fx->params;
    const bool below
        = p.staticValue(k::TrackSource, fxchoice::TrackClip).toString() == fxchoice::TrackBelow;
    if (below && trackIndex == 0)
        return fail(QStringLiteral("there is no track below the clip"));
    const bool planar
        = p.staticValue(k::TrackKind, fxchoice::KindPosition).toString() == fxchoice::KindPlanar;

    EffectTrackPlan plan;
    plan.clipId = clipId;
    plan.effectId = effectId;
    plan.signature = matchmove::signature(p);
    plan.stride = planar ? matchmove::kRegionStride : matchmove::kPointStride;
    plan.startFrame = playhead;
    const Rational fps = tl.fps;
    const ClipData clipCopy = *clip;
    plan.secondOf = [clipCopy, fps](int64_t frame) {
        return double(clipCopy.sourceTimeUsAt(frame, fps)) / 1e6;
    };
    plan.referenceSecond = plan.secondOf(playhead);

    // The private timeline.
    auto copy = std::make_shared<TimelineData>(tl);
    for (size_t t = 0; t < copy->videoTracks.size(); ++t) {
        TrackData& track = copy->videoTracks[t];
        if (below) {
            if (int(t) >= trackIndex)
                track.clips.clear();
        } else {
            std::erase_if(track.clips, [clipId](const ClipData& c) { return c.id != clipId; });
        }
    }
    if (!below) {
        for (TrackData& track : copy->subtitleTracks)
            track.clips.clear();
        for (TrackData& track : copy->videoTracks) {
            for (ClipData& c : track.clips) {
                for (EffectInstance& e : c.effects) {
                    if (e.type == QLatin1String(fxid::MatchMove))
                        e.enabled = false;
                }
            }
        }
    } else {
        for (TrackData& track : copy->subtitleTracks)
            track.clips.clear();
    }
    plan.timeline = copy;

    const int width = std::min(kTrackWidth, tl.width);
    plan.renderSize = QSize(width, std::max(2, int(std::lround(double(width) * tl.height / tl.width))));

    TrackJob& job = plan.job;
    job.firstFrame = playhead;
    job.lastFrame = forward ? clip->start + clip->duration - 1 : clip->start;
    const double size = std::clamp(p.value(k::RegionSize, 0, 12.0), 2.0, 80.0) / 100.0;
    const double u = std::clamp(p.value(k::RegionX, 0, 50.0), 0.0, 100.0) / 100.0;
    const double v = 1.0 - std::clamp(p.value(k::RegionY, 0, 50.0), 0.0, 100.0) / 100.0;
    job.startU = u;
    job.startV = v;
    job.planar = planar;
    if (planar) {
        const double hw = size / 2;
        const double hh = hw * double(tl.width) / double(tl.height);
        job.startCorners = {u - hw, v + hh, u + hw, v + hh, u + hw, v - hh, u - hw, v - hh};
        job.patternFraction = std::max(0.02, size / 4);
        job.searchFraction = std::max(0.12, size * 1.5);
    } else {
        job.patternFraction = std::max(0.02, size);
        job.searchFraction = std::max(0.12, size * 3);
    }
    return plan;
}

std::shared_ptr<const efftrack::Table> tableFromSamples(
    const EffectTrackPlan& plan, const std::vector<TrackSample>& samples)
{
    if (samples.empty())
        return nullptr;
    std::vector<const TrackSample*> sorted;
    for (const TrackSample& s : samples)
        sorted.push_back(&s);
    std::sort(sorted.begin(), sorted.end(), [&](const TrackSample* a, const TrackSample* b) {
        return plan.secondOf(a->frame) < plan.secondOf(b->frame);
    });
    std::vector<double> seconds;
    std::vector<float> values;
    for (const TrackSample* s : sorted) {
        const double second = plan.secondOf(s->frame);
        if (!seconds.empty() && std::abs(seconds.back() - second) < 1e-9)
            continue; // a retimed clip can show one source time twice
        seconds.push_back(second);
        if (plan.stride == matchmove::kPointStride) {
            values.push_back(float(s->u));
            values.push_back(float(s->v));
        } else {
            for (const double c : s->corners)
                values.push_back(float(c));
        }
    }
    return std::make_shared<efftrack::Table>(
        plan.signature, plan.referenceSecond, plan.stride, std::move(seconds), std::move(values));
}

bool storeEffectTrack(ClipData& clip, quint64 effectId, const efftrack::Table& table)
{
    EffectInstance* fx = findEffect(clip, effectId);
    if (!fx)
        return false;
    const auto old = efftrack::decode(fx->params.staticValue(k::TrackData).toString());
    const auto result = efftrack::merged(old.get(), table);
    fx->params.setStatic(k::TrackData, efftrack::encode(*result));
    return true;
}

bool clearEffectTrack(ClipData& clip, quint64 effectId)
{
    EffectInstance* fx = findEffect(clip, effectId);
    if (!fx)
        return false;
    fx->params.setStatic(k::TrackData, QString());
    return true;
}

} // namespace kk::tracking
