// SPDX-License-Identifier: GPL-3.0-or-later
// Plain value types describing a timeline: clips, tracks, markers, effects and transitions.
// These are copied freely (undo snapshots, render snapshots); keep them cheap and logic-free.
#pragma once

#include "core/Animation.h"
#include "core/Rational.h"

#include <QColor>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>
#include <vector>

namespace cw {

/// An effect applied to a clip. `type` is an EffectRegistry descriptor id (e.g. "gaussian_blur").
struct EffectInstance
{
    quint64 id = 0; ///< unique within the timeline (allocated with TimelineData::allocateId())
    QString type;
    bool enabled = true;
    ParamSet params; ///< keys from the descriptor's ParamDescriptor list

    friend bool operator==(const EffectInstance&, const EffectInstance&) = default;
};

/// A transition attached to the head (transitionIn) or tail (transitionOut) of a clip.
/// Semantics are described in PLAN §5.4 (centred on the cut if there is an adjacent neighbour,
/// otherwise a fade from/to transparent).
struct TransitionData
{
    QString type = QStringLiteral("cross_dissolve"); ///< EffectRegistry id (category Transition)
    int64_t duration = 25; ///< length in timeline frames
    ParamSet params;

    friend bool operator==(const TransitionData&, const TransitionData&) = default;
};

/// What a clip shows.
enum class ClipKind {
    Media, ///< a MediaItem from the media pool (video and/or audio)
    Title, ///< a text title generated from ClipData::title
    Solid ///< a solid colour (ClipData::solidColor)
};

/// One clip on a track. Positions are timeline frames, source positions microseconds (PLAN §5.1).
struct ClipData
{
    quint64 id = 0; ///< unique within the timeline; 0 = not yet placed (EditOps assign one)
    ClipKind kind = ClipKind::Media;
    QString mediaId; ///< MediaItem::id for ClipKind::Media
    QString name;
    QColor label; ///< optional user colour label (invalid = none)
    int64_t start = 0; ///< first timeline frame
    int64_t duration = 0; ///< length in timeline frames (>= 1 once on a track)
    int64_t sourceInUs = 0; ///< source media time shown at `start`
    /// Duration of the source media in µs, used to clamp trims. 0 = unbounded (stills, titles,
    /// solids). Filled from MediaItem::info.durationUs when the clip is created.
    int64_t sourceDurationUs = 0;
    double speed = 1.0; ///< playback speed factor (> 0)
    bool enabled = true;
    quint64 linkGroup = 0; ///< V+A clips from the same source share a non-zero group id

    ParamSet transform; ///< keys::PosX ... keys::CropB, statics keys::FlipH/FlipV/ZoomLinked
    ParamSet audio; ///< keys::VolumeDb, keys::Pan
    ParamSet title; ///< Title clips only: see the title keys in core/ParamKeys.h
    QColor solidColor; ///< Solid clips only
    QVector<EffectInstance> effects; ///< applied in order
    std::optional<TransitionData> transitionIn;
    std::optional<TransitionData> transitionOut;

    /// First frame after the clip.
    [[nodiscard]] int64_t end() const { return start + duration; }
    /// True if timeline frame @p frame lies inside [start, end).
    [[nodiscard]] bool contains(int64_t frame) const { return frame >= start && frame < end(); }
    /// Clip-local frame (used for keyframes) of timeline frame @p frame.
    [[nodiscard]] int64_t localFrame(int64_t frame) const { return frame - start; }
    /// Source media microseconds covered by @p frames timeline frames at this clip's speed.
    [[nodiscard]] int64_t framesToSourceUs(int64_t frames, Rational fps) const;
    /// Source media time (µs) displayed at timeline frame @p frame (PLAN §5.1 formula).
    [[nodiscard]] int64_t sourceTimeUsAt(int64_t frame, Rational fps) const
    {
        return sourceInUs + framesToSourceUs(frame - start, fps);
    }

    friend bool operator==(const ClipData&, const ClipData&) = default;
};

/// Video or audio track.
enum class TrackType {
    Video,
    Audio
};

/// One track. Invariant: `clips` sorted by start, never overlapping (see editops::checkInvariants).
struct TrackData
{
    quint64 id = 0; ///< unique within the timeline
    TrackType type = TrackType::Video;
    QString name; ///< user name; empty = automatic ("V1", "A2", see trackDisplayName())
    bool enabled = true; ///< video: visible in the output; audio: unused (see muted)
    bool locked = false; ///< locked tracks are never modified by EditOps
    bool muted = false;
    bool solo = false;
    int height = 0; ///< UI track height in px, 0 = default
    std::vector<ClipData> clips;

    /// Clip covering timeline frame @p frame, or nullptr.
    [[nodiscard]] const ClipData* clipAt(int64_t frame) const;
    /// Index of the clip with id @p clipId in `clips`, or -1.
    [[nodiscard]] int indexOfClip(quint64 clipId) const;
    /// First frame after the last clip (0 for an empty track).
    [[nodiscard]] int64_t end() const { return clips.empty() ? 0 : clips.back().end(); }

    friend bool operator==(const TrackData&, const TrackData&) = default;
};

/// A timeline marker.
struct Marker
{
    int64_t frame = 0;
    QString name;
    QColor color = QColor(0x4c, 0x9b, 0xe8);

    friend bool operator==(const Marker&, const Marker&) = default;
};

/// Addresses a track by type and index (index 0 = V1/A1; V1 is the bottom-most video track).
struct TrackRef
{
    TrackType type = TrackType::Video;
    int index = 0;

    friend bool operator==(const TrackRef&, const TrackRef&) = default;
};

/// The complete, copyable state of a timeline (PLAN §5.2).
struct TimelineData
{
    QString name;
    Rational fps; ///< timeline frame rate; all frame values are in this rate
    int width = 1920;
    int height = 1080;
    std::vector<TrackData> videoTracks; ///< index 0 = V1 (bottom-most)
    std::vector<TrackData> audioTracks; ///< index 0 = A1
    int64_t markIn = -1; ///< -1 = not set
    int64_t markOut = -1; ///< -1 = not set; exclusive end frame
    QVector<Marker> markers; ///< sorted by frame
    quint64 nextId = 1; ///< id allocator for clips, tracks and effect instances

    /// End of the last clip on any track, in frames.
    [[nodiscard]] int64_t duration() const;
    /// Video or audio track list.
    [[nodiscard]] std::vector<TrackData>& tracks(TrackType type)
    {
        return type == TrackType::Video ? videoTracks : audioTracks;
    }
    /// Video or audio track list (const).
    [[nodiscard]] const std::vector<TrackData>& tracks(TrackType type) const
    {
        return type == TrackType::Video ? videoTracks : audioTracks;
    }
    /// Track at @p ref, or nullptr if out of range.
    [[nodiscard]] TrackData* track(TrackRef ref);
    /// Track at @p ref, or nullptr if out of range (const).
    [[nodiscard]] const TrackData* track(TrackRef ref) const;
    /// Returns a fresh id, unique within this timeline.
    quint64 allocateId() { return nextId++; }

    friend bool operator==(const TimelineData&, const TimelineData&) = default;
};

/// Display name of a track: its user name, or "V<n>" / "A<n>" from the 0-based @p index.
[[nodiscard]] QString trackDisplayName(const TrackData& track, int index);

/// A new timeline with two video (V1, V2) and two audio (A1, A2) tracks.
[[nodiscard]] TimelineData makeDefaultTimeline(
    const QString& name, Rational fps, int width = 1920, int height = 1080);

} // namespace cw
