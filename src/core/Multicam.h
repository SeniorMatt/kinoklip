// SPDX-License-Identifier: GPL-3.0-or-later
// Multicam — pure functions for multicam clips: build the multicam timeline from synced
// sources, angle views for rendering, the viewer's angle grid, cut / switch angle edits,
// flatten, and timecode maths for "sync by timecode".
#pragma once

#include "core/CompoundOps.h"
#include "core/TimelineData.h"
#include "media/MediaInfo.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace kk::multicam {

/// Most angles a multicam clip can have (the viewer grid shows at most 4x4 per page).
inline constexpr int kMaxAngles = 16;

/// How the angles of a new multicam clip (or "sync and place on timeline") are aligned.
enum class SyncMethod {
    InPoints, ///< the source in points line up (no in point = clip start)
    Timecode, ///< embedded start timecode (MediaInfo::startTimecode)
    Waveform, ///< common sound (media/AudioSync.h, cross-correlation)
    Start ///< every source starts at the same time (no sync)
};

/// Which part of a multicam clip an angle edit changes (Resolve's video / audio / both
/// buttons of the multicam viewer).
enum class AngleTarget {
    Both,
    Video,
    Audio
};

/// One synced source of a new multicam clip.
struct AngleSource
{
    QString mediaId; ///< MediaItem::id of the clip
    QString name; ///< angle name (track name of the angle)
    media::MediaInfo info; ///< the item's probe result (duration, video / audio, rate)
    /// Where media time 0 of this source lies on the common sync axis (µs, any origin; only
    /// the differences between sources matter). See offsetsFrom*().
    int64_t offsetUs = 0;
};

/// Number of angles of multicam timeline @p mc (its video and audio track count, whichever is
/// larger; angle k = video track k + audio track k).
[[nodiscard]] int angleCount(const TimelineData& mc);

/// Display name of angle @p angle (0-based): the angle's video track name, else its audio
/// track name, else "Angle n".
[[nodiscard]] QString angleName(const TimelineData& mc, int angle);

/// The timeline a Nested clip with ClipData::multicamAngle = @p angle shows: @p mc with only
/// video track @p angle and audio track @p angle (each possibly empty), same uid, rate and
/// size. Used for the angle entries of render snapshots (multicamViewKey()).
[[nodiscard]] TimelineData angleView(const TimelineData& mc, int angle);

/// Angle name for a source: the camera name of the file's metadata if any (not probed yet),
/// else the media pool name without its extension.
[[nodiscard]] QString defaultAngleName(const QString& itemName);

/// Builds the timeline of a new multicam clip (uid @p uid, @p name, rate @p fps, picture
/// @p width x @p height; multicam + compound set): source k on video track k (if it has
/// video) and audio track k (if it has audio, linked to its video), track names = angle
/// names. Positions follow AngleSource::offsetUs: the earliest source starts at frame 0,
/// every start is rounded up to a whole frame and the sub-frame rest is taken from the
/// clip's head (sourceInUs), so the sources stay aligned to the microsecond.
/// Fails (nullopt, @p error) for fewer than one or more than kMaxAngles sources or sources
/// without a duration (stills).
[[nodiscard]] std::optional<TimelineData> build(const QVector<AngleSource>& sources, quint64 uid,
    const QString& name, Rational fps, int width, int height, QString* error = nullptr);

/// A Nested clip showing all of @p mc from its start at angle @p angle (id 0, start 0), for
/// drops of the multicam clip onto a timeline (video and audio parts are linked copies).
[[nodiscard]] ClipData makeMulticamClip(const TimelineData& mc, const QString& mediaId, int angle);

/// True if @p clip is a Nested clip showing a multicam timeline that @p lookup knows.
[[nodiscard]] bool isMulticamClip(const ClipData& clip, const compound::TimelineLookup& lookup);

/// The multicam clip under timeline frame @p frame: the top-most video track first, then the
/// audio tracks from A1 (with @p preferAudio, audio first). 0 if none.
[[nodiscard]] quint64 multicamClipAt(const TimelineData& data, int64_t frame,
    const compound::TimelineLookup& lookup, bool preferAudio = false);

/// @p clipId and the clips linked to it that show the same multicam timeline, split into the
/// video and audio parts.
struct MulticamParts
{
    QVector<quint64> video;
    QVector<quint64> audio;
};
[[nodiscard]] MulticamParts parts(const TimelineData& data, quint64 clipId);

/// Switch: the multicam clip @p clipId (and its linked parts per @p target) shows angle
/// @p angle from now on, over its whole length. Returns false (nothing changed) if the clip
/// is not a multicam clip, the angle does not exist, a track is locked or nothing changes.
bool switchAngle(TimelineData& data, quint64 clipId, int angle, AngleTarget target,
    const compound::TimelineLookup& lookup);

/// Cut: like Resolve's multicam viewer in cut mode. The multicam clip under @p frame (see
/// multicamClipAt(); with AngleTarget::Audio its audio part first) is split at @p frame and
/// the part from @p frame on shows angle @p angle (video / audio / both per @p target; the
/// other side is left uncut). On the first frame of a segment no cut is made, the segment
/// is switched. A part already showing @p angle is not cut. Split parts stay in the link
/// group of the original. Returns the ids of the clips that now show @p angle (empty =
/// nothing changed).
QVector<quint64> cutToAngle(TimelineData& data, int64_t frame, int angle, AngleTarget target,
    const compound::TimelineLookup& lookup);

/// Flatten Multicam Clip: replaces the multicam clips @p clipIds (and their linked parts) by
/// the clips of their active angle, cut to the visible window (compound::decompose() on the
/// angle view). The restored video and audio of one angle are linked again. Fails (nothing
/// changed) like decompose (speed changed, other frame rate) or for non-multicam clips.
[[nodiscard]] std::optional<QVector<quint64>> flatten(TimelineData& data,
    const QVector<quint64>& clipIds, const compound::TimelineLookup& lookup, QString* error);

/// The viewer's angle grid as a timeline (same rate and size as @p mc): page @p page of a
/// @p columns x @p columns grid; each cell is a Nested clip of one angle scaled to 1 /
/// columns and moved into its cell (row-major, angle 1 top left), and one audio track plays
/// @p audioAngle. Played by the source viewer's engine, so the angles decode, play and stay
/// in sync like any nested timeline. Cells keep the angle's aspect ratio.
[[nodiscard]] TimelineData gridTimeline(
    const TimelineData& mc, int columns, int page, int audioAngle, const QString& mediaId);

/// Grid columns that show @p angles angles on one page: 2 (up to 4), 3 (up to 9), else 4.
[[nodiscard]] int columnsFor(int angles);

/// Angle (0-based) of cell (@p column, @p row) of page @p page of a @p columns grid, or -1
/// when the cell is empty.
[[nodiscard]] int angleAtCell(int angles, int columns, int page, int column, int row);

// ---- Sync offsets ----

/// Parses an SMPTE timecode "HH:MM:SS:FF" (or "HH:MM:SS;FF" / "HH;MM;SS;FF" = drop frame,
/// '.' accepted too) at @p fps into microseconds from 00:00:00:00. The frame rate of the
/// timecode is @p fps rounded to whole frames (29.97 -> 30 counting); drop frame removes
/// frame numbers 0 and 1 of every minute except each tenth (29.97 / 59.94 only).
/// nullopt for malformed text or an invalid rate.
[[nodiscard]] std::optional<int64_t> timecodeToUs(const QString& timecode, Rational fps);

/// AngleSource::offsetUs per source for SyncMethod::Timecode: the start timecode in µs.
/// Sources without a (parseable) timecode get nullopt.
[[nodiscard]] QVector<std::optional<int64_t>> offsetsFromTimecode(
    const QVector<media::MediaInfo>& infos);

/// AngleSource::offsetUs per source for SyncMethod::InPoints: in point k (source µs) lines up
/// with every other in point, i.e. offset = -inUs.
[[nodiscard]] QVector<int64_t> offsetsFromInPoints(const QVector<int64_t>& inPointsUs);

} // namespace kk::multicam
