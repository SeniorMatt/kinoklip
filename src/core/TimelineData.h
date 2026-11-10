// SPDX-License-Identifier: GPL-3.0-or-later
// Plain value types describing a timeline: clips, tracks, markers, effects and transitions.
// These are copied freely (undo snapshots, render snapshots); keep them cheap and logic-free.
#pragma once

#include "core/Animation.h"
#include "core/ClipAttributes.h"
#include "core/Composition.h"
#include "core/Grade.h"
#include "core/Mask.h"
#include "core/MixerData.h"
#include "core/Rational.h"
#include "core/Stabilization.h"
#include "core/ViewerGuides.h"

#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace kk {

struct TimelineData;

/// An effect applied to a clip. `type` is an EffectRegistry descriptor id (e.g. "gaussian_blur").
struct EffectInstance
{
    quint64 id = 0; ///< unique within the timeline (allocated with TimelineData::allocateId())
    QString type;
    bool enabled = true;
    ParamSet params; ///< keys from the descriptor's ParamDescriptor list

    friend bool operator==(const EffectInstance&, const EffectInstance&) = default;
};

/// Where a transition between two touching clips sits relative to the cut (spec 13.5).
enum class TransitionAlignment {
    Centre, ///< half before, half after the cut (default)
    StartOnCut, ///< starts at the cut: all frames in the incoming clip (needs outgoing handles)
    EndOnCut ///< ends at the cut: all frames in the outgoing clip (needs incoming handles)
};

/// A transition attached to the head (transitionIn) or tail (transitionOut) of a clip.
/// Semantics are described in PLAN §5.4 (placed on the cut by `alignment` if there is an
/// adjacent neighbour, otherwise a fade from/to transparent).
struct TransitionData
{
    QString type = QStringLiteral("cross_dissolve"); ///< EffectRegistry id (category Transition)
    int64_t duration = 25; ///< length in timeline frames
    TransitionAlignment alignment = TransitionAlignment::Centre;
    ParamSet params;

    /// Frames of the transition that lie before the cut (at a cut between two clips).
    [[nodiscard]] int64_t framesBeforeCut() const
    {
        switch (alignment) {
        case TransitionAlignment::StartOnCut:
            return 0;
        case TransitionAlignment::EndOnCut:
            return duration;
        case TransitionAlignment::Centre:
            break;
        }
        return duration / 2;
    }

    friend bool operator==(const TransitionData&, const TransitionData&) = default;
};

/// A marker: on the timeline (TimelineData::markers, timeline frames) or on a clip
/// (ClipData::markers, clip-local frames that follow the content like keyframes; spec 18.4).
struct Marker
{
    int64_t frame = 0;
    QString name;
    QColor color = QColor(0x4c, 0x9b, 0xe8);
    QString note; ///< free text (marker editor, spec 18.5)
    /// Length in frames (>= 1): 1 = a single-frame marker, more = a ranged marker covering
    /// [frame, frame + duration) (spec 18.11). Saved only when > 1 (format 13).
    int64_t duration = 1;

    /// First frame after the marker's range.
    [[nodiscard]] int64_t end() const { return frame + duration; }

    friend bool operator==(const Marker&, const Marker&) = default;
};

/// How a clip playing at another speed than its source produces the frames between two source
/// frames (Resolve's "retime process"). ProjectDefault defers to TimelineData::retimeProcess.
enum class RetimeProcess : quint8 {
    ProjectDefault, ///< clips only: use the timeline's (project's) default
    Nearest, ///< the source frame covering the source time (no interpolation)
    FrameBlend, ///< cross-fade of the two source frames around the source time
    OpticalFlow ///< motion-compensated interpolation (GPU block-matching flow, render/GlRetime)
};

/// What a title layer draws (spec section 68).
enum class TitleLayerKind {
    Text,
    Shape
};

/// One extra layer of a Title clip (core/TitleLayers.h): its parameters use the keys of
/// core/ParamKeys.h (a Text layer the text keys, a Shape layer the shape keys).
struct TitleLayer
{
    quint64 id = 0; ///< unique within the clip, never 0 (0 is the first layer, ClipData::title)
    TitleLayerKind kind = TitleLayerKind::Text;
    QString name; ///< empty = the default name (core/TitleLayers.h displayName())
    bool visible = true;
    ParamSet params;

    friend bool operator==(const TitleLayer&, const TitleLayer&) = default;
};

/// What the first layer of a Title clip (ClipData::title) has besides its parameters.
struct TitleBaseLayer
{
    int slot = 0; ///< how many extra layers lie below it (0 = it is the bottom layer)
    bool visible = true;
    QString name;

    friend bool operator==(const TitleBaseLayer&, const TitleBaseLayer&) = default;
};

/// What a clip shows.
enum class ClipKind {
    Media, ///< a MediaItem from the media pool (video and/or audio)
    Title, ///< a text title generated from ClipData::title
    Solid, ///< a solid colour (ClipData::solidColor)
    /// Another timeline of the project used as a clip: a compound clip or a nested timeline
    /// (ClipData::nestedUid). Its "source" time is the nested timeline's time in µs, so
    /// trim / slip / split / speed work exactly as for media.
    Nested,
    /// An adjustment clip (Effects > Adjustment Clip): no picture of its own. Its effects,
    /// transform, crop, opacity and masks act on the composite of every video track below it,
    /// within its time range (src/render/readme.md, "Adjustment clips").
    Adjustment,
    /// A generated picture (ClipData::generator = EffectRegistry id of category Generator,
    /// e.g. "four_color_gradient"; its parameters live in ClipData::title).
    Generator,
    /// A caption on a subtitle track: text and style overrides in ClipData::title (keys::Text
    /// and the subtitle style keys), drawn over the picture with the track's default style.
    Subtitle
};

/// True for clip kinds without source media (titles, solids, generators, adjustment clips,
/// captions): unbounded trims, no decoding.
[[nodiscard]] inline bool isGeneratedKind(ClipKind kind)
{
    return kind != ClipKind::Media && kind != ClipKind::Nested;
}

/// One clip on a track. Positions are timeline frames, source positions microseconds (PLAN §5.1).
struct ClipData
{
    quint64 id = 0; ///< unique within the timeline; 0 = not yet placed (EditOps assign one)
    ClipKind kind = ClipKind::Media;
    QString mediaId; ///< MediaItem::id for ClipKind::Media (and the pool item of a Nested clip)
    /// ClipKind::Nested: TimelineData::uid of the timeline the clip shows (authoritative; a
    /// uid that no longer exists makes the clip offline: it draws and plays nothing).
    quint64 nestedUid = 0;
    /// ClipKind::Nested showing a multicam timeline (TimelineData::multicam): the 0-based
    /// angle the clip shows (video clips: the video track of that angle, audio clips: its
    /// audio track). -1 = no angle: the whole nested timeline (every other Nested clip).
    int multicamAngle = -1;
    QString name;
    QColor label; ///< optional user colour label (invalid = none)
    int64_t start = 0; ///< first timeline frame
    int64_t duration = 0; ///< length in timeline frames (>= 1 once on a track)
    int64_t sourceInUs = 0; ///< source media time shown at `start`
    /// Duration of the source media in µs, used to clamp trims. 0 = unbounded (stills, titles,
    /// solids). Filled from MediaItem::info.durationUs when the clip is created.
    int64_t sourceDurationUs = 0;
    /// Constant playback speed used while the clip has no retime curve (core/Retime.h):
    /// 1 = normal, 0.5 = half speed, < 0 = reverse, 0 = freeze frame. Finite.
    double speed = 1.0;
    /// Retime curve: keys::RetimeFrame keyframed with two or more keys replaces `speed` (speed
    /// points, ramps, freeze and reverse segments; see core/Retime.h). Clip-local frames like
    /// every keyframe.
    ParamSet retime;
    RetimeProcess retimeProcess = RetimeProcess::ProjectDefault;
    /// Audio of a clip that is not at 100 % keeps its pitch (time stretch) instead of being
    /// resampled (pitch follows the speed).
    bool pitchCorrection = true;
    bool enabled = true;
    quint64 linkGroup = 0; ///< V+A clips from the same source share a non-zero group id

    ParamSet transform; ///< keys::PosX ... keys::CropB, statics keys::FlipH/FlipV/ZoomLinked
    ParamSet audio; ///< keys::VolumeDb, keys::Pan
    /// Title clips: the title keys in core/ParamKeys.h. Generator clips: the parameters of the
    /// generator descriptor. Subtitle clips: keys::Text plus the style keys that override the
    /// track's default style (core/Subtitles.h).
    ParamSet title;
    /// Title clips: more layers drawn over (or, per titleBase.slot, under) the first layer,
    /// which is `title`. Bottom to top. Empty for every title made before layers existed.
    QVector<TitleLayer> titleLayers;
    TitleBaseLayer titleBase;
    /// Generator clips only: EffectRegistry descriptor id of the generator.
    QString generator;
    QColor solidColor; ///< Solid clips only
    QVector<EffectInstance> effects; ///< applied in order
    /// Shape masks, evaluated in order on the clip's picture (after the effects, before the
    /// transform); see core/Mask.h. Ids share the timeline's id space.
    QVector<MaskData> masks;
    std::optional<TransitionData> transitionIn;
    std::optional<TransitionData> transitionOut;
    /// Clip markers, sorted by frame; clip-local frames (frame - start), may lie outside the
    /// visible part after a trim (kept like keyframes so undoing nothing is lost).
    QVector<Marker> markers;
    /// Clip > Render Cache Clip Output: the render cache keeps this clip's frames in User
    /// mode (src/render/RenderCacheKeys.h). Not part of the cache key itself.
    bool cacheOutput = false;
    /// Fade handles (spec section 31): frames of the fade-in from the clip start and of the
    /// fade-out to the clip end; 0 = none. Video: linear opacity ramp; audio: equal-power gain
    /// ramp (core/ClipAttributes.h). They multiply with transitions and the keyframed opacity /
    /// volume, are kept by trims, and a split / range cut drops the fade at the cut.
    int64_t fadeInFrames = 0;
    int64_t fadeOutFrames = 0;
    /// Fade curves (spec section 59): the shape of each fade, -1 ... +1, 0 = linear (the
    /// default, and what older projects load as). Positive = slow start (the level stays low
    /// at first and rises late), negative = fast start (see core/ClipAttributes.h fadeShape()).
    double fadeInCurve = 0.0;
    double fadeOutCurve = 0.0;
    /// Media clips: the Clip Attributes of the media pool item (MediaItem::attributes),
    /// mirrored by Project::syncDerived() so render snapshots need no media pool lookup.
    ClipAttributes attributes;
    /// Compositing page node graph (core/Composition.h). When set, the clip shows the
    /// composition's output instead of its source picture; the clip's effects, masks,
    /// transform, opacity and compositing then apply on top. std::nullopt = no composition
    /// (the clip's picture straight through). Keyframes in it are clip-local frames.
    std::optional<Composition> composition;
    /// Color page grade (core/Grade.h, docs/color-page.md): applied to the clip's picture
    /// after its composition and Edit page effects, before its masks, crop, transform and
    /// opacity. Empty = ungraded. Node and window ids are local to the grade; keyframes are
    /// clip-local frames like every clip key.
    ClipGrade grade;
    /// Stabilization (core/Stabilization.h, spec section 58): the Inspector's settings and the
    /// measured camera path, keyed by source frame. Applied before Dynamic Zoom and the clip's
    /// own transform. Default (off, no data) = a clip that never had it.
    stabilize::ClipStabilization stabilization;

    /// First frame after the clip.
    [[nodiscard]] int64_t end() const { return start + duration; }
    /// True if timeline frame @p frame lies inside [start, end).
    [[nodiscard]] bool contains(int64_t frame) const { return frame >= start && frame < end(); }
    /// Clip-local frame (used for keyframes) of timeline frame @p frame.
    [[nodiscard]] int64_t localFrame(int64_t frame) const { return frame - start; }
    /// Source media microseconds covered by @p frames timeline frames at the constant `speed`
    /// (ignores a retime curve; see retime::sourceOffsetUs() for the general form).
    [[nodiscard]] int64_t framesToSourceUs(int64_t frames, Rational fps) const;
    /// Source media time (µs) displayed at timeline frame @p frame: PLAN §5.1 formula
    /// `sourceIn + offset` where the offset follows the retime curve (core/Retime.h) or the
    /// constant speed. Not clamped to the media.
    [[nodiscard]] int64_t sourceTimeUsAt(int64_t frame, Rational fps) const;

    friend bool operator==(const ClipData&, const ClipData&) = default;
};

/// Video, audio or subtitle track. Subtitle tracks are drawn above every video track (in the
/// timeline and over the picture) and hold ClipKind::Subtitle clips only.
enum class TrackType {
    Video,
    Audio,
    Subtitle
};

/// Number of TrackType values (size of per-type arrays indexed by trackTypeIndex()).
inline constexpr int kTrackTypeCount = 3;
/// Dense index of @p type: 0 = video, 1 = audio, 2 = subtitle.
[[nodiscard]] constexpr int trackTypeIndex(TrackType type)
{
    return int(type);
}

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
    /// Auto track selector: the track takes part in edits that are not aimed at a specific
    /// clip (previous/next edit point, split at the playhead without a selection, mark clip,
    /// default transition) and in the all-track ripple of insert edits and gap removal.
    bool autoSelect = true;
    int height = 0; ///< UI track height in px, 0 = default
    /// Subtitle tracks only: the default caption style (subtitle style keys of
    /// core/Subtitles.h); a caption's own ClipData::title keys override it.
    ParamSet subtitleStyle;
    /// Subtitle tracks only: ISO 639 language code written to sidecar / embedded subtitle
    /// streams (empty = undefined).
    QString language;
    /// Audio tracks only: the mixer channel (EQ, dynamics, pan, fader, channel format, bus
    /// routing, automation; Audio page, core/MixerData.h).
    TrackMix mix;
    std::vector<ClipData> clips;

    /// Clip covering timeline frame @p frame, or nullptr.
    [[nodiscard]] const ClipData* clipAt(int64_t frame) const;
    /// Index of the clip with id @p clipId in `clips`, or -1.
    [[nodiscard]] int indexOfClip(quint64 clipId) const;
    /// First frame after the last clip (0 for an empty track).
    [[nodiscard]] int64_t end() const { return clips.empty() ? 0 : clips.back().end(); }

    friend bool operator==(const TrackData&, const TrackData&) = default;
};

/// Addresses a track by type and index (index 0 = V1/A1; V1 is the bottom-most video track).
struct TrackRef
{
    TrackType type = TrackType::Video;
    int index = 0;

    friend bool operator==(const TrackRef&, const TrackRef&) = default;
};

/// Nested timelines of a render snapshot: TimelineData::uid -> immutable data (see
/// TimelineData::nested). Every timeline reachable from the snapshot is in the one map; the
/// entries themselves carry no map (renderers pass the root's map down).
/// Multicam timelines also contribute one entry per angle (multicamViewKey()): a copy that
/// holds only that angle's video and audio track (core/Multicam.h, multicam::angleView()).
using NestedTimelineMap = QHash<quint64, std::shared_ptr<const TimelineData>>;

/// Key of the angle view of multicam timeline @p uid, angle @p angle (0-based) in a
/// NestedTimelineMap. Timeline uids stay far below 2^40, so the keys never collide with them.
[[nodiscard]] constexpr quint64 multicamViewKey(quint64 uid, int angle)
{
    return uid | (quint64(angle + 1) << 40);
}
/// The timeline-uid bits of a NestedTimelineMap key (removes the angle of multicamViewKey()).
inline constexpr quint64 kTimelineUidMask = (quint64(1) << 40) - 1;

/// Holder of the nested-timeline map of a render snapshot. Compares equal to every other
/// holder: it is derived data (filled by Timeline::snapshot()), so it must never make two
/// otherwise identical timelines differ (undo snapshots, change detection).
struct NestedTimelines
{
    std::shared_ptr<const NestedTimelineMap> map;

    /// The nested timeline with @p uid, or nullptr.
    [[nodiscard]] const TimelineData* find(quint64 uid) const
    {
        if (!map)
            return nullptr;
        const auto it = map->constFind(uid);
        return it == map->constEnd() ? nullptr : it->get();
    }
    /// What Nested clip @p clip shows: for a multicam angle (ClipData::multicamAngle >= 0)
    /// the angle view of its multicam timeline (multicamViewKey()), else (or when the map has
    /// no such view) the nested timeline itself. nullptr if neither exists (offline).
    [[nodiscard]] const TimelineData* findClip(const ClipData& clip) const;
    friend bool operator==(const NestedTimelines&, const NestedTimelines&) { return true; }
};

/// The complete, copyable state of a timeline (PLAN §5.2).
struct TimelineData
{
    /// Project-unique, stable id of the timeline (referenced by ClipData::nestedUid and
    /// MediaItem::timelineUid); 0 = a stand-alone timeline that is not part of a project.
    quint64 uid = 0;
    /// True for the timeline behind a compound clip (Clip > New Compound Clip): listed in the
    /// media pool as a compound clip, not as a timeline.
    bool compound = false;
    /// True for the timeline behind a multicam clip (also `compound`, so it is never counted
    /// as a regular timeline): video track k and audio track k are angle k (core/Multicam.h),
    /// listed in the media pool as a multicam clip.
    bool multicam = false;
    /// Resolution and frame rate follow the project settings (Project::setSettings()).
    bool useProjectSettings = true;
    QString name;
    Rational fps; ///< timeline frame rate; all frame values are in this rate
    /// Starting timecode: the record timecode of frame 0, in frames of `fps` (Timeline
    /// Settings; 01:00:00:00 for new timelines of new projects, 0 in files without it; spec
    /// 25.12). Display only: every frame value stays 0-based; the ruler, viewers, Edit Index,
    /// markers, typed timecodes, EDL export and burn-ins add it.
    int64_t startTimecode = 0;
    int width = 1920;
    int height = 1080;
    std::vector<TrackData> videoTracks; ///< index 0 = V1 (bottom-most)
    std::vector<TrackData> audioTracks; ///< index 0 = A1
    std::vector<TrackData> subtitleTracks; ///< index 0 = ST1 (lowest, drawn first)
    /// Retime process of clips set to RetimeProcess::ProjectDefault (Project Settings > Master
    /// Settings > Frame Interpolation; Project::setSettings() keeps every timeline in sync).
    RetimeProcess retimeProcess = RetimeProcess::Nearest;
    int64_t markIn = -1; ///< -1 = not set
    int64_t markOut = -1; ///< -1 = not set; exclusive end frame
    QVector<Marker> markers; ///< sorted by frame
    quint64 nextId = 1; ///< id allocator for clips, tracks and effect instances
    /// Buses and the output bus of the Audio page's mixer (core/MixerData.h).
    MixerData mixer;
    /// Viewer guide lines of this timeline (View > Guides; core/ViewerGuides.h). Display
    /// only: the renderers never read them, so they never appear in a render or export.
    std::vector<ViewerGuide> guides;
    /// Timeline grade (Color page): applied to the composite of every video track, after the
    /// clip grades and before the captions and the output. Keyframes in timeline frames.
    ClipGrade grade;
    /// Render snapshots only (Timeline::snapshot()): every timeline the nested clips of this
    /// one reach, directly or indirectly. Empty in editable data; never serialized.
    NestedTimelines nested;

    /// End of the last clip on any track, in frames.
    [[nodiscard]] int64_t duration() const;
    /// Video, audio or subtitle track list.
    [[nodiscard]] std::vector<TrackData>& tracks(TrackType type)
    {
        return type == TrackType::Video ? videoTracks
            : type == TrackType::Audio  ? audioTracks
                                        : subtitleTracks;
    }
    /// Video, audio or subtitle track list (const).
    [[nodiscard]] const std::vector<TrackData>& tracks(TrackType type) const
    {
        return type == TrackType::Video ? videoTracks
            : type == TrackType::Audio  ? audioTracks
                                        : subtitleTracks;
    }
    /// Track at @p ref, or nullptr if out of range.
    [[nodiscard]] TrackData* track(TrackRef ref);
    /// Track at @p ref, or nullptr if out of range (const).
    [[nodiscard]] const TrackData* track(TrackRef ref) const;
    /// Returns a fresh id, unique within this timeline.
    quint64 allocateId() { return nextId++; }

    friend bool operator==(const TimelineData&, const TimelineData&) = default;
};

/// Timeline frame of the nested timeline (frame rate @p nestedFps) shown by Nested clip
/// @p clip at frame @p frame of its parent (rate @p parentFps). Not clamped: frames outside
/// the nested timeline show nothing. Floor rounding like usToFrames().
[[nodiscard]] int64_t nestedFrameAt(
    const ClipData& clip, int64_t frame, Rational parentFps, Rational nestedFps);

/// Display name of a track: its user name, or "V<n>" / "A<n>" / "ST<n>" from the 0-based
/// @p index.
[[nodiscard]] QString trackDisplayName(const TrackData& track, int index);

/// Whole frames per timecode second of @p fps (29.97 -> 30, 23.976 -> 24; 25 if invalid).
[[nodiscard]] int timecodeBase(Rational fps);

/// 01:00:00:00 at @p fps, in frames: the starting timecode of new timelines (spec 25.12).
[[nodiscard]] int64_t oneHourTimecode(Rational fps);

/// The same "HH:MM:SS:FF" timecode at @p to as @p frames is at @p from (a starting timecode
/// when the frame rate changes; frames beyond the new rate's last frame number are clamped).
[[nodiscard]] int64_t convertTimecode(int64_t frames, Rational from, Rational to);

/// A new timeline with @p videoTracks video and @p audioTracks audio tracks: one of each (V1,
/// A1) by default, as a new Resolve timeline (spec 25.14). Older projects keep their tracks.
[[nodiscard]] TimelineData makeDefaultTimeline(const QString& name, Rational fps, int width = 1920,
    int height = 1080, int videoTracks = 1, int audioTracks = 1);

} // namespace kk
