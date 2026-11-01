// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineEval — transition windows, per-track evaluation and clip geometry (pure functions).
#include "render/TimelineEval.h"

#include "core/ParamKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

/// Left neighbour of clips[index] if it ends exactly where the clip starts.
const ClipData* adjacentPrev(const TrackData& track, int index)
{
    if (index <= 0)
        return nullptr;
    const ClipData& prev = track.clips[size_t(index - 1)];
    return prev.end() == track.clips[size_t(index)].start ? &prev : nullptr;
}

/// Right neighbour of clips[index] if it starts exactly where the clip ends.
const ClipData* adjacentNext(const TrackData& track, int index)
{
    if (index + 1 >= int(track.clips.size()))
        return nullptr;
    const ClipData& next = track.clips[size_t(index + 1)];
    return next.start == track.clips[size_t(index)].end() ? &next : nullptr;
}

/// Window centred on the cut between @p a and @p b: [cut - d/2, cut - d/2 + d).
TransitionSpan centredSpan(const TransitionData& t, const ClipData& a, const ClipData& b)
{
    const int64_t d = std::max<int64_t>(1, t.duration);
    TransitionSpan span;
    span.transition = &t;
    span.start = b.start - d / 2;
    span.end = span.start + d;
    span.centred = true;
    span.outgoing = &a;
    span.incoming = &b;
    return span;
}

} // namespace

std::optional<TransitionSpan> headTransition(const TrackData& track, int index)
{
    const ClipData& clip = track.clips[size_t(index)];
    const ClipData* prev = adjacentPrev(track, index);
    if (clip.transitionIn) {
        if (prev)
            return centredSpan(*clip.transitionIn, *prev, clip);
        TransitionSpan span; // fade in from transparent over the first d frames
        span.transition = &*clip.transitionIn;
        span.start = clip.start;
        span.end = clip.start + std::clamp<int64_t>(clip.transitionIn->duration, 1, clip.duration);
        span.incoming = &clip;
        return span;
    }
    if (prev && prev->transitionOut)
        return centredSpan(*prev->transitionOut, *prev, clip);
    return std::nullopt;
}

std::optional<TransitionSpan> tailTransition(const TrackData& track, int index)
{
    const ClipData& clip = track.clips[size_t(index)];
    const ClipData* next = adjacentNext(track, index);
    if (next && next->transitionIn) // the incoming clip's transitionIn wins at a shared cut
        return centredSpan(*next->transitionIn, clip, *next);
    if (!clip.transitionOut)
        return std::nullopt;
    if (next)
        return centredSpan(*clip.transitionOut, clip, *next);
    TransitionSpan span; // fade out to transparent over the last d frames
    span.transition = &*clip.transitionOut;
    span.end = clip.end();
    span.start = span.end - std::clamp<int64_t>(clip.transitionOut->duration, 1, clip.duration);
    span.outgoing = &clip;
    return span;
}

int clipIndexAt(const TrackData& track, int64_t frame)
{
    // First clip starting after `frame`; the candidate is the one before it.
    const auto it = std::upper_bound(track.clips.begin(), track.clips.end(), frame,
        [](int64_t f, const ClipData& c) { return f < c.start; });
    if (it == track.clips.begin())
        return -1;
    const int index = int(it - track.clips.begin()) - 1;
    return track.clips[size_t(index)].contains(frame) ? index : -1;
}

TrackFrame evaluateTrack(const TrackData& track, int64_t frame)
{
    TrackFrame result;
    const int index = clipIndexAt(track, frame);
    if (index < 0)
        return result;
    if (auto head = headTransition(track, index); head && head->contains(frame)) {
        result.transition = head;
        return result;
    }
    if (auto tail = tailTransition(track, index); tail && tail->contains(frame)) {
        result.transition = tail;
        return result;
    }
    result.single = &track.clips[size_t(index)];
    return result;
}

int64_t clampedSourceTimeUs(const ClipData& clip, int64_t frame, Rational fps)
{
    int64_t t = clip.sourceTimeUsAt(frame, fps);
    if (clip.sourceDurationUs > 0)
        t = std::min(t, clip.sourceDurationUs - 1);
    return std::max<int64_t>(0, t);
}

QSizeF fitSize(QSizeF source, QSizeF frame)
{
    if (source.width() <= 0 || source.height() <= 0)
        return frame;
    const double s = std::min(frame.width() / source.width(), frame.height() / source.height());
    return QSizeF(source.width() * s, source.height() * s);
}

QTransform clipTransform(const ClipData& clip, int64_t localFrame, QSizeF frameSize)
{
    const ParamSet& p = clip.transform;
    const double posX = p.value(keys::PosX, localFrame);
    const double posY = p.value(keys::PosY, localFrame);
    double zoomX = p.value(keys::ZoomX, localFrame);
    double zoomY = p.value(keys::ZoomY, localFrame);
    const double rotation = p.value(keys::Rotation, localFrame);
    const double anchorX = p.value(keys::AnchorX, localFrame);
    const double anchorY = p.value(keys::AnchorY, localFrame);
    if (p.statics.value(keys::FlipH).toBool())
        zoomX = -zoomX;
    if (p.statics.value(keys::FlipV).toBool())
        zoomY = -zoomY;

    // QTransform composes so that the LAST call is applied FIRST to a point; read bottom-up.
    QTransform m;
    m.translate(
        frameSize.width() * 0.5 + posX + anchorX, frameSize.height() * 0.5 + posY + anchorY);
    m.rotate(rotation); // y-down coordinates: positive angle = clockwise on screen
    m.scale(zoomX, zoomY);
    m.translate(-anchorX, -anchorY);
    return m;
}

double maxZoom(const ClipData& clip, int64_t localFrame)
{
    return std::max(std::abs(clip.transform.value(keys::ZoomX, localFrame)),
        std::abs(clip.transform.value(keys::ZoomY, localFrame)));
}

QSizeF displaySize(int codedWidth, int codedHeight, Rational sar, int rotation)
{
    const double sarValue = sar.isValid() ? sar.toDouble() : 1.0;
    const QSizeF s(codedWidth * sarValue, codedHeight);
    return (rotation == 90 || rotation == 270) ? s.transposed() : s;
}

} // namespace kk::render
