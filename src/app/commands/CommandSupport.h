// SPDX-License-Identifier: GPL-3.0-or-later
// CommandSupport — helpers shared by the command implementations (not part of the public layer):
// track names, JSON descriptions of timelines and clips, id lists.
#pragma once

#include "app/commands/Command.h"
#include "core/TimelineData.h"

namespace kk {
class Project;
}

namespace kk::commands {

/// Shorthand for CommandResult::failure().
[[nodiscard]] inline CommandResult fail(const char* code, const QString& message)
{
    return CommandResult::failure(QString::fromLatin1(code), message);
}

/// "V1", "A2" (case does not matter) to a track of @p data; std::nullopt when the name is not
/// of that form or the track does not exist. Subtitle tracks are not addressable.
[[nodiscard]] std::optional<TrackRef> parseTrack(const TimelineData& data, const QString& name);

/// "V1" / "A2" for a track address.
[[nodiscard]] QString trackName(TrackRef ref);

/// Whole numbers in @p texts, each of which may hold several separated by commas or spaces
/// ("3,4"); sets @p error and returns an empty list when one is not a number.
[[nodiscard]] QVector<quint64> parseIds(const QStringList& texts, QString* error);

/// A clip as JSON: id, name, kind, track, start / duration / end in frames and as timecode,
/// source in / out in microseconds, media id, speed, enabled, volumeDb, transform values.
[[nodiscard]] QJsonObject clipToJson(
    const TimelineData& data, TrackRef track, const ClipData& clip);

/// A timeline as JSON: id, name, frame rate, size, track counts, clip count, duration.
[[nodiscard]] QJsonObject timelineToJson(const TimelineData& data, bool active);

/// Reads Time parameter @p name in frames of @p fps into @p out (left unchanged when the
/// parameter is absent). @return a failure for text that is not a time, else std::nullopt.
[[nodiscard]] std::optional<CommandResult> readTime(
    const CommandArgs& args, const QString& name, Rational fps, std::optional<int64_t>& out);

/// The clip @p id of @p data with its track, or a failure ("unknown_clip").
struct FoundClip
{
    TrackRef track;
    const ClipData* clip = nullptr;
};
[[nodiscard]] std::optional<CommandResult> findClipOrFail(
    const TimelineData& data, quint64 id, FoundClip& found);

/// The 24 / 25 / 30 / 60 style frame rate as an exact rational: 23.976 becomes 24000/1001
/// (also 29.97, 59.94, ...), whole numbers n/1, anything else thousandths.
[[nodiscard]] Rational rationalFromFps(double fps);

} // namespace kk::commands
