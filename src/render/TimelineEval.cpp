// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineEval — transition windows, per-track evaluation and clip geometry (pure functions).
#include "render/TimelineEval.h"

#include "core/DynamicZoom.h"
#include "core/NodeRegistry.h"
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

/// Window on the cut between @p a and @p b, placed by the transition's alignment:
/// [cut - before, cut - before + d) with before = d/2 (centre), 0 (start on cut) or d (end on
/// cut), spec 13.5.
TransitionSpan centredSpan(const TransitionData& t, const ClipData& a, const ClipData& b)
{
    const int64_t d = std::max<int64_t>(1, t.duration);
    TransitionSpan span;
    span.transition = &t;
    span.start = b.start - std::clamp<int64_t>(t.framesBeforeCut(), 0, d);
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

QTransform clipTransform(
    const ClipData& clip, int64_t localFrame, QSizeF frameSize, bool dynamicZoom, Rational fps)
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
    // Applied first: Stabilization, then Dynamic Zoom, then the transform above.
    if (dynamicZoom && dynamiczoom::isEnabled(p))
        m = dynamiczoom::transform(clip, localFrame, frameSize) * m;
    if (stabilize::isActive(clip.stabilization) && fps.isValid())
        m = stabilize::transform(clip, localFrame, frameSize, fps) * m;
    return m;
}

double maxZoom(const ClipData& clip, int64_t localFrame, bool dynamicZoom)
{
    const double zoom = std::max(std::abs(clip.transform.value(keys::ZoomX, localFrame)),
        std::abs(clip.transform.value(keys::ZoomY, localFrame)));
    const double stabilized = zoom * stabilize::zoomOf(clip);
    if (!dynamicZoom || !dynamiczoom::isEnabled(clip.transform))
        return stabilized;
    return stabilized / std::max(dynamiczoom::kMinSize, dynamiczoom::at(clip, localFrame).size);
}

QSizeF displaySize(int codedWidth, int codedHeight, Rational sar, int rotation)
{
    const double sarValue = sar.isValid() ? sar.toDouble() : 1.0;
    const QSizeF s(codedWidth * sarValue, codedHeight);
    return (rotation == 90 || rotation == 270) ? s.transposed() : s;
}

int compositeModeIndex(const TrackFrame& tf, int64_t frame)
{
    const ClipData* clip = tf.single;
    if (tf.transition) {
        const TransitionSpan& span = *tf.transition;
        clip = span.progress(frame) < 0.5 ? span.outgoing : span.incoming;
        if (!clip)
            clip = span.outgoing ? span.outgoing : span.incoming;
    }
    if (!clip || clip->kind == ClipKind::Adjustment)
        return 0;
    const QVariant mode = clip->transform.staticValue(keys::CompositeMode);
    return mode.isValid() ? blendmodes::index(mode.toString()) : 0;
}

double clipOpacity(const ClipData& clip, int64_t localFrame)
{
    const double opacity
        = std::clamp(clip.transform.value(keys::Opacity, localFrame) / 100.0, 0.0, 1.0);
    if (clip.fadeInFrames <= 0 && clip.fadeOutFrames <= 0)
        return opacity;
    return opacity
        * videoFadeFactor(localFrame, clip.duration, clip.fadeInFrames, clip.fadeOutFrames);
}

bool hasIdentityFootprint(const ClipData& clip, int64_t localFrame, bool dynamicZoom)
{
    if (clipOpacity(clip, localFrame) < 1.0)
        return false;
    if (stabilize::isActive(clip.stabilization))
        return false;
    if (dynamicZoom && dynamiczoom::isEnabled(clip.transform)
        && dynamiczoom::at(clip, localFrame) != dynamiczoom::Rect {})
        return false;
    const ParamSet& p = clip.transform;
    if (p.value(keys::PosX, localFrame) != 0.0 || p.value(keys::PosY, localFrame) != 0.0
        || p.value(keys::ZoomX, localFrame) != 1.0 || p.value(keys::ZoomY, localFrame) != 1.0
        || p.value(keys::Rotation, localFrame) != 0.0 || p.value(keys::Opacity, localFrame) < 100.0
        || p.value(keys::CropL, localFrame) > 0.0 || p.value(keys::CropR, localFrame) > 0.0
        || p.value(keys::CropT, localFrame) > 0.0 || p.value(keys::CropB, localFrame) > 0.0
        || p.staticValue(keys::FlipH).toBool() || p.staticValue(keys::FlipV).toBool())
        return false;
    return std::none_of(
        clip.masks.cbegin(), clip.masks.cend(), [](const MaskData& m) { return m.isRenderable(); });
}

bool sameNestedTimelines(const NestedTimelines& a, const NestedTimelines& b)
{
    // NestedTimelines::operator== is always true (derived data must not make timelines
    // differ), so the maps are compared here. Their entries are the project's shared timeline
    // states: an unchanged nested timeline keeps its pointer, an edited one gets a new one.
    if (a.map == b.map)
        return true;
    const qsizetype sizeA = a.map ? a.map->size() : 0;
    const qsizetype sizeB = b.map ? b.map->size() : 0;
    if (sizeA != sizeB)
        return false;
    if (sizeA == 0)
        return true;
    for (auto it = a.map->cbegin(); it != a.map->cend(); ++it) {
        const auto other = b.map->constFind(it.key());
        if (other == b.map->cend() || other.value() != it.value())
            return false;
    }
    return true;
}

namespace {

/// The fields of a clip that cannot change a picture are ignored (a link, a colour, a flag or
/// a marker during playback must not cancel the frames being rendered).
bool samePictureClip(const ClipData& a, const ClipData& b)
{
    if (a.id != b.id || a.start != b.start || a.duration != b.duration)
        return false;
    if (a == b)
        return true;
    ClipData x = a;
    ClipData y = b;
    for (ClipData* c : {&x, &y}) {
        c->name.clear();
        c->label = QColor();
        c->markers.clear();
        c->linkGroup = 0;
        c->cacheOutput = false;
        c->audio = ParamSet();
    }
    return x == y;
}

bool samePictureTracks(const std::vector<TrackData>& a, const std::vector<TrackData>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const TrackData& x = a[i];
        const TrackData& y = b[i];
        if (x.id != y.id || x.enabled != y.enabled || x.muted != y.muted || x.solo != y.solo
            || x.clips.size() != y.clips.size())
            return false;
        for (size_t k = 0; k < x.clips.size(); ++k) {
            if (!samePictureClip(x.clips[k], y.clips[k]))
                return false;
        }
    }
    return true;
}

bool sameTransitionPlace(
    const std::optional<TransitionData>& a, const std::optional<TransitionData>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    return !a || (a->type == b->type && a->duration == b->duration && a->alignment == b->alignment);
}

/// Clip fields that decide which sound or picture plays when.
bool samePlace(const ClipData& a, const ClipData& b)
{
    return a.id == b.id && a.kind == b.kind && a.mediaId == b.mediaId && a.nestedUid == b.nestedUid
        && a.start == b.start && a.duration == b.duration && a.sourceInUs == b.sourceInUs
        && a.speed == b.speed && a.enabled == b.enabled && a.retime == b.retime;
}

bool sameArrangedTracks(const std::vector<TrackData>& a, const std::vector<TrackData>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].enabled != b[i].enabled
            || a[i].clips.size() != b[i].clips.size())
            return false;
        for (size_t k = 0; k < a[i].clips.size(); ++k) {
            const ClipData& x = a[i].clips[k];
            const ClipData& y = b[i].clips[k];
            if (!samePlace(x, y) || x.linkGroup != y.linkGroup
                || !sameTransitionPlace(x.transitionIn, y.transitionIn)
                || !sameTransitionPlace(x.transitionOut, y.transitionOut))
                return false;
        }
    }
    return true;
}

/// The clips of @p track overlapping [@p from, @p to), in order.
std::vector<const ClipData*> clipsIn(const TrackData* track, int64_t from, int64_t to)
{
    std::vector<const ClipData*> out;
    if (track) {
        for (const ClipData& c : track->clips) {
            if (c.start < to && c.end() > from)
                out.push_back(&c);
        }
    }
    return out;
}

} // namespace

bool samePictures(const TimelineData& a, const TimelineData& b)
{
    return a.uid == b.uid && a.fps == b.fps && a.width == b.width && a.height == b.height
        && a.retimeProcess == b.retimeProcess && samePictureTracks(a.videoTracks, b.videoTracks)
        && a.subtitleTracks == b.subtitleTracks && a.grade == b.grade
        && sameNestedTimelines(a.nested, b.nested);
}

bool sameArrangement(const TimelineData& a, const TimelineData& b)
{
    return a.uid == b.uid && sameArrangedTracks(a.videoTracks, b.videoTracks)
        && sameArrangedTracks(a.audioTracks, b.audioTracks)
        && sameArrangedTracks(a.subtitleTracks, b.subtitleTracks);
}

bool soundArrangementDiffers(const TimelineData& a, const TimelineData& b, int64_t from, int64_t to)
{
    const size_t tracks = std::max(a.audioTracks.size(), b.audioTracks.size());
    for (size_t i = 0; i < tracks; ++i) {
        const auto x = clipsIn(i < a.audioTracks.size() ? &a.audioTracks[i] : nullptr, from, to);
        const auto y = clipsIn(i < b.audioTracks.size() ? &b.audioTracks[i] : nullptr, from, to);
        if (x.size() != y.size())
            return true;
        for (size_t k = 0; k < x.size(); ++k) {
            if (!samePlace(*x[k], *y[k]))
                return true;
        }
    }
    return false;
}

} // namespace kk::render
