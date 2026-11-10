// SPDX-License-Identifier: GPL-3.0-or-later
// EffectTrackJob — the Track buttons of the tracking effects (Match Move, Surface Tracker;
// spec section 70). planEffectTrack() turns the effect's parameters and the playhead into a
// TrackJob for the TrackerRunner plus the private copy of the timeline whose frames are tracked:
// "This Clip" renders the clip alone as the viewer shows it (without the effect), "Footage Below"
// renders the video tracks under the clip's track. The frames come from the playback engine
// (PlaybackEngine::renderImageOf()), so what is tracked is what is seen. The finished samples
// become a core/EffectTrack.h table keyed by the clip's source time, merged into the stored one
// when it was tracked from the same place.
#pragma once

#include "compositing/tracking/TrackerRunner.h"
#include "core/EffectTrack.h"
#include "core/TimelineData.h"

#include <QSize>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

namespace kk::tracking {

/// A planned track of one effect of one clip.
struct EffectTrackPlan
{
    TrackJob job; ///< timeline frames; `source` is left for the caller
    std::shared_ptr<const TimelineData> timeline; ///< what to render for the job's frames
    QSize renderSize; ///< size of the pictures to track
    QString signature; ///< what the table will be a track of
    int stride = 0; ///< floats per key
    int64_t startFrame = 0; ///< the timeline frame the track starts on (the reference)
    double referenceSecond = 0;
    quint64 clipId = 0;
    quint64 effectId = 0;
    /// Source time (seconds) of timeline frame @p frame of the job's clip.
    std::function<double(int64_t frame)> secondOf;
};

/// Plans tracking effect @p effectId of clip @p clipId from timeline frame @p playhead forwards
/// or backwards to the end or start of the clip. nullopt with a sentence in @p why when it
/// cannot be tracked (unknown clip or effect, playhead outside the clip, a clip that is not
/// picture, nothing below).
[[nodiscard]] std::optional<EffectTrackPlan> planEffectTrack(const TimelineData& timeline,
    quint64 clipId, quint64 effectId, int64_t playhead, bool forward, QString* why);

/// The table of @p samples (in the order they were found) for @p plan.
[[nodiscard]] std::shared_ptr<const efftrack::Table> tableFromSamples(
    const EffectTrackPlan& plan, const std::vector<TrackSample>& samples);

/// Writes @p table into effect @p effectId of @p clip (merged with the stored table when it has
/// the same signature and reference). False if the effect is missing.
bool storeEffectTrack(ClipData& clip, quint64 effectId, const efftrack::Table& table);
/// Removes the stored track of the effect. False if the effect is missing.
bool clearEffectTrack(ClipData& clip, quint64 effectId);

/// The clip with id @p clipId and the index of its video track; nullptr when it is not on a
/// video track.
[[nodiscard]] const ClipData* findVideoClip(
    const TimelineData& timeline, quint64 clipId, int* trackIndex = nullptr);

} // namespace kk::tracking
