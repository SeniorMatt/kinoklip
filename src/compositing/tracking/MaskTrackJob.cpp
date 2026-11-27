// SPDX-License-Identifier: GPL-3.0-or-later
#include "compositing/tracking/MaskTrackJob.h"

#include "compositing/tracking/TrackFollow.h"
#include "core/EffectRegistry.h"
#include "core/EffectTrack.h"
#include "core/Mask.h"
#include "core/MatchMove.h"
#include "core/VideoEffectKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::tracking {

namespace {

namespace k = fxkeys;

const MaskData* findMask(const ClipData& clip, quint64 id)
{
    for (const MaskData& m : clip.masks) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

} // namespace

QRectF maskBounds(const MaskData& mask, int64_t localFrame)
{
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    auto grow = [&](QPointF local) {
        const QPointF p = maskToContent(mask, localFrame, local);
        if (!any) {
            x0 = x1 = p.x();
            y0 = y1 = p.y();
            any = true;
            return;
        }
        x0 = std::min(x0, p.x());
        x1 = std::max(x1, p.x());
        y0 = std::min(y0, p.y());
        y1 = std::max(y1, p.y());
    };
    if (mask.isPathShape()) {
        const MaskPath path = mask.path.at(double(localFrame));
        if (path.size() < 2)
            return {};
        for (const MaskPoint& pt : path)
            grow(pt.pos());
    } else {
        const double hw = mask.value(maskkeys::Width, localFrame) / 2;
        const double hh = mask.value(maskkeys::Height, localFrame) / 2;
        if (hw <= 0 || hh <= 0)
            return {};
        for (const QPointF c :
            {QPointF(-hw, -hh), QPointF(hw, -hh), QPointF(hw, hh), QPointF(-hw, hh)})
            grow(c);
    }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

std::optional<MaskTrackPlan> planMaskTrack(const TimelineData& tl, quint64 clipId, quint64 maskId,
    int64_t playhead, bool forward, QString* why)
{
    auto fail = [why](const QString& text) -> std::optional<MaskTrackPlan> {
        if (why)
            *why = text;
        return std::nullopt;
    };
    const ClipData* clip = findVideoClip(tl, clipId);
    if (!clip)
        return fail(QStringLiteral("the clip is not on a video track"));
    const MaskData* mask = findMask(*clip, maskId);
    if (!mask)
        return fail(QStringLiteral("the mask is missing"));
    if (!clip->contains(playhead))
        return fail(QStringLiteral("move the playhead into the clip first"));
    if (tl.width <= 0 || tl.height <= 0)
        return fail(QStringLiteral("the timeline has no frame size"));
    const int64_t local = clip->localFrame(playhead);
    const QRectF bounds = maskBounds(*mask, local);
    if (bounds.isNull() && bounds.width() <= 0 && bounds.height() <= 0 && !mask->isPathShape())
        return fail(QStringLiteral("the mask has no shape yet"));
    if (mask->isPathShape() && mask->path.pointCount() < 2)
        return fail(QStringLiteral("the mask has no shape yet"));

    // The tracked region is a square about the mask's middle, a little larger than the mask so
    // that its edge texture is in it; a big mask is limited to 80 % of the picture width.
    const double side = std::max({bounds.width(), bounds.height(), 24.0}) * 1.1;
    const double sizePercent = std::clamp(100.0 * side / double(tl.width), 2.0, 80.0);
    const double half = sizePercent / 100.0 * double(tl.width) / 2;
    const QPointF c = bounds.center();

    // A private copy of the timeline with a planar Match Move on the clip (it only carries the
    // region) and without the clip's masks.
    TimelineData copy = tl;
    ClipData* target = nullptr;
    for (TrackData& track : copy.videoTracks) {
        for (ClipData& cd : track.clips) {
            if (cd.id == clipId)
                target = &cd;
        }
    }
    if (!target)
        return fail(QStringLiteral("the clip is not on a video track"));
    target->masks.clear();
    EffectInstance fx = EffectRegistry::instance().createEffect(
        QLatin1String(fxid::MatchMove), copy.allocateId());
    fx.params.setBase(k::RegionX, 100.0 * (0.5 + c.x() / double(tl.width)));
    fx.params.setBase(k::RegionY, 100.0 * (0.5 + c.y() / double(tl.height)));
    fx.params.setBase(k::RegionSize, sizePercent);
    fx.params.setStatic(k::TrackKind, fxchoice::KindPlanar);
    fx.params.setStatic(k::TrackSource, fxchoice::TrackClip);
    const quint64 effectId = fx.id;
    target->effects.push_back(fx);

    QString inner;
    auto track = planEffectTrack(copy, clipId, effectId, playhead, forward, &inner);
    if (!track)
        return fail(inner);
    MaskTrackPlan plan;
    plan.track = std::move(*track);
    // A big mask is followed by many small patches of its texture, not a few huge ones: a patch
    // a third of the region wide matches too much of a busy picture, and the search box of a
    // Match Move region (1.5 times its size) would let the wrong match win.
    plan.track.job.patternFraction = std::clamp(side / double(tl.width) / 6.0, 0.025, 0.06);
    plan.track.job.searchFraction = 0.16;
    plan.clipId = clipId;
    plan.maskId = maskId;
    plan.offsetFrame = local;
    plan.region = QRectF(c.x() - half, c.y() - half, 2 * half, 2 * half);
    return plan;
}

bool storeMaskTrack(ClipData& clip, const MaskTrackPlan& plan,
    const std::vector<TrackSample>& samples, QSizeF frameSize, Rational fps)
{
    if (samples.size() < 2)
        return false;
    MaskData* mask = nullptr;
    for (MaskData& m : clip.masks) {
        if (m.id == plan.maskId)
            mask = &m;
    }
    const auto table = tableFromSamples(plan.track, samples);
    if (!mask || !table)
        return false;
    EffectInstance fx;
    fx.type = QLatin1String(fxid::MatchMove);
    fx.params.setStatic(k::TrackData, efftrack::encode(*table));
    FollowOptions o;
    o.position = o.size = o.angle = true;
    return followMatchMove(*mask, clip, fx, plan.offsetFrame, frameSize, fps, o);
}

} // namespace kk::tracking
