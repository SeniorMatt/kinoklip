// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineEval — pure functions answering "what is visible/audible at frame f": transition
// windows (PLAN §5.4 semantics), the clips a track contributes, and clip geometry. Shared by
// the GPU compositor, the CPU fallback and the AudioMixer so all three agree exactly.
#pragma once

#include "core/TimelineData.h"

#include <QSizeF>
#include <QTransform>

#include <cstdint>
#include <optional>

namespace kk::render {

/// A transition affecting one edge (head or tail) of a clip, in timeline frames.
///
/// Semantics (PLAN §5.4):
/// * `transitionIn` on clip B whose left neighbour A ends exactly at B.start: the transition
///   is centred on the cut, window [cut - d/2, cut - d/2 + d). A keeps playing past its end
///   (source handles; the decoder clamps = freeze on the last frame when there are none), B
///   starts before its start (freeze on the first frame without handles).
/// * Without an adjacent neighbour: a fade from transparent over the clip's first d frames.
/// * `transitionOut` likewise at the tail (fade to transparent over the last d frames).
/// * If A has a transitionOut AND B a transitionIn at the same cut, B's transitionIn wins.
struct TransitionSpan
{
    const TransitionData* transition = nullptr;
    int64_t start = 0; ///< first frame of the window
    int64_t end = 0; ///< first frame after the window
    bool centred = false; ///< true: between two clips; false: fade from/to transparent
    const ClipData* outgoing = nullptr; ///< clip A (nullptr for a fade-in)
    const ClipData* incoming = nullptr; ///< clip B (nullptr for a fade-out)

    [[nodiscard]] bool contains(int64_t frame) const { return frame >= start && frame < end; }
    [[nodiscard]] int64_t length() const { return end - start; }
    /// Progress at @p frame: 0 = all outgoing, 1 = all incoming. Exclusive at both ends,
    /// (f - start + 1) / (d + 1), so every frame inside the window shows a blend and the
    /// frames just outside it show the pure clips.
    [[nodiscard]] double progress(int64_t frame) const
    {
        return double(frame - start + 1) / double(length() + 1);
    }
};

/// Transition at the head of `track.clips[index]` (its own transitionIn, or the left
/// neighbour's centred transitionOut), if any.
[[nodiscard]] std::optional<TransitionSpan> headTransition(const TrackData& track, int index);
/// Transition at the tail of `track.clips[index]` (its own transitionOut, or the right
/// neighbour's centred transitionIn), if any.
[[nodiscard]] std::optional<TransitionSpan> tailTransition(const TrackData& track, int index);

/// What one track shows at one frame.
///
/// * Plain clip: `single` set, `transition` empty.
/// * Transition: `transition` set; its outgoing/incoming pointers are the two clips (one may
///   be nullptr = transparent for fades at track ends). `single` is nullptr.
/// * Nothing (gap / empty track): both empty.
/// Disabled clips are still reported (the renderer draws them as transparent), so a
/// transition next to a disabled clip still fades.
struct TrackFrame
{
    const ClipData* single = nullptr;
    std::optional<TransitionSpan> transition;

    [[nodiscard]] bool isEmpty() const { return !single && !transition; }
};

/// Evaluates @p track at timeline frame @p frame. O(log clips), allocation-free.
[[nodiscard]] TrackFrame evaluateTrack(const TrackData& track, int64_t frame);

/// Index of the clip covering @p frame, or -1. O(log n).
[[nodiscard]] int clipIndexAt(const TrackData& track, int64_t frame);

/// Opacity of @p clip at clip-local frame @p localFrame (0..1): its keyframed Opacity times
/// the fade handles (videoFadeFactor()). Shared by the GPU and CPU renderers.
[[nodiscard]] double clipOpacity(const ClipData& clip, int64_t localFrame);

/// Source media time (µs) shown by @p clip at timeline frame @p frame, clamped to the media
/// (>= 0 and < sourceDurationUs when that is known). Frames outside the clip (transition
/// handles) extrapolate and therefore freeze on the first/last frame when there is no media.
[[nodiscard]] int64_t clampedSourceTimeUs(const ClipData& clip, int64_t frame, Rational fps);

/// Largest size with the aspect ratio of @p source fitting into @p frame ("scale to fit":
/// what a media clip occupies at zoom 1). Empty source → @p frame.
[[nodiscard]] QSizeF fitSize(QSizeF source, QSizeF frame);

/// Maps clip content coordinates (timeline pixels, origin at the CENTRE of the content,
/// y down) to timeline frame pixels (origin top-left, y down) for @p clip at clip-local frame
/// @p localFrame. Implements anchor, zoom X/Y (+ flipH/flipV), rotation (degrees clockwise)
/// and position:
///
///     p_frame = frameCentre + pos + anchor + R(rot) · S(zoom·flip) · (p_content − anchor)
///
/// i.e. the anchor point is the pivot of zoom/rotation and stays where the position puts it,
/// so changing only the anchor never moves an unscaled, unrotated picture.
[[nodiscard]] QTransform clipTransform(const ClipData& clip, int64_t localFrame, QSizeF frameSize);

/// True if @p clip at @p localFrame covers the frame exactly as its content: no move, zoom,
/// rotation, flip or crop, opacity 100 % and no renderable mask. An adjustment clip with an
/// identity footprint replaces the whole frame (and does nothing when its effects do nothing).
[[nodiscard]] bool hasIdentityFootprint(const ClipData& clip, int64_t localFrame);

/// Largest |zoom| of @p clip at @p localFrame (used to choose the layer resolution).
[[nodiscard]] double maxZoom(const ClipData& clip, int64_t localFrame);

/// Display size of a coded picture: width × SAR, then swapped for 90/270° rotation.
[[nodiscard]] QSizeF displaySize(int codedWidth, int codedHeight, Rational sar, int rotation);

/// True if @p a and @p b render the same pictures: they differ at most in what only the
/// sound uses (audio tracks and their clips, the mixer, markers, marks). Conservative: any
/// change of a video or subtitle track, the format or a nested timeline counts as different.
[[nodiscard]] bool samePictures(const TimelineData& a, const TimelineData& b);

/// True if the nested-timeline maps of two render snapshots hold the same timeline states
/// (same uids, same shared TimelineData). NestedTimelines::operator== cannot tell: it is
/// always true by design.
[[nodiscard]] bool sameNestedTimelines(const NestedTimelines& a, const NestedTimelines& b);

} // namespace kk::render
