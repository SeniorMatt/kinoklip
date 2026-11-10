// SPDX-License-Identifier: GPL-3.0-or-later
// CommandSupport implementation.
#include "app/commands/CommandSupport.h"

#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "ui/common/TimecodeFormat.h"

#include <QRegularExpression>

#include <cmath>

namespace kk::commands {

std::optional<TrackRef> parseTrack(const TimelineData& data, const QString& name)
{
    const QString n = name.trimmed().toUpper();
    if (n.size() < 2)
        return std::nullopt;
    TrackType type;
    if (n[0] == QLatin1Char('V'))
        type = TrackType::Video;
    else if (n[0] == QLatin1Char('A'))
        type = TrackType::Audio;
    else
        return std::nullopt;
    bool ok = false;
    const int number = n.mid(1).toInt(&ok);
    if (!ok || number < 1 || number > int(data.tracks(type).size()))
        return std::nullopt;
    return TrackRef {type, number - 1};
}

QString trackName(TrackRef ref)
{
    const char* prefix = ref.type == TrackType::Video ? "V"
        : ref.type == TrackType::Audio                ? "A"
                                                      : "ST";
    return QString::fromLatin1(prefix) + QString::number(ref.index + 1);
}

QVector<quint64> parseIds(const QStringList& texts, QString* error)
{
    QVector<quint64> ids;
    for (const QString& text : texts) {
        for (const QString& part :
            text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)) {
            bool ok = false;
            const quint64 id = part.toULongLong(&ok);
            if (!ok) {
                if (error)
                    *error = QStringLiteral("\"%1\" is not a clip id").arg(part);
                return {};
            }
            ids.append(id);
        }
    }
    return ids;
}

QJsonObject clipToJson(const TimelineData& data, TrackRef track, const ClipData& clip)
{
    const Rational fps = data.fps;
    const double volume = clip.audio.value(keys::VolumeDb, 0);
    const int64_t sourceOutUs = clip.sourceInUs
        + int64_t(std::llround(double(framesToUs(clip.duration, fps)) * clip.speed));
    QJsonObject transform;
    for (const QString& key :
        {keys::PosX, keys::PosY, keys::ZoomX, keys::ZoomY, keys::Rotation, keys::Opacity})
        transform.insert(key, clip.transform.value(key, 0));
    QJsonObject o {{QStringLiteral("id"), qint64(clip.id)}, {QStringLiteral("name"), clip.name},
        {QStringLiteral("track"), trackName(track)}, {QStringLiteral("start"), qint64(clip.start)},
        {QStringLiteral("duration"), qint64(clip.duration)},
        {QStringLiteral("end"), qint64(clip.end())},
        {QStringLiteral("startTimecode"), timecode::format(clip.start, fps)},
        {QStringLiteral("durationTimecode"), timecode::format(clip.duration, fps)},
        {QStringLiteral("sourceInUs"), qint64(clip.sourceInUs)},
        {QStringLiteral("sourceOutUs"), qint64(sourceOutUs)},
        {QStringLiteral("mediaId"), clip.mediaId}, {QStringLiteral("speed"), clip.speed},
        {QStringLiteral("enabled"), clip.enabled}, {QStringLiteral("volumeDb"), volume},
        {QStringLiteral("transform"), transform},
        {QStringLiteral("linkGroup"), qint64(clip.linkGroup)}};
    static const char* const kinds[]
        = {"media", "title", "solid", "nested", "adjustment", "generator", "subtitle"};
    o.insert(QStringLiteral("kind"), QString::fromLatin1(kinds[int(clip.kind)]));
    if (clip.transitionIn)
        o.insert(QStringLiteral("transitionIn"),
            QJsonObject {{QStringLiteral("type"), clip.transitionIn->type},
                {QStringLiteral("duration"), qint64(clip.transitionIn->duration)}});
    if (clip.transitionOut)
        o.insert(QStringLiteral("transitionOut"),
            QJsonObject {{QStringLiteral("type"), clip.transitionOut->type},
                {QStringLiteral("duration"), qint64(clip.transitionOut->duration)}});
    if (!clip.stabilization.isDefault()) {
        const stabilize::Settings& st = clip.stabilization.settings;
        o.insert(QStringLiteral("stabilization"),
            QJsonObject {{QStringLiteral("enabled"), st.enabled},
                {QStringLiteral("mode"), stabilize::modeId(st.mode)},
                {QStringLiteral("cameraLock"), st.cameraLock}, {QStringLiteral("zoom"), st.zoom},
                {QStringLiteral("croppingRatio"), st.croppingRatio},
                {QStringLiteral("smooth"), st.smooth}, {QStringLiteral("strength"), st.strength},
                {QStringLiteral("analysed"), bool(clip.stabilization.data)}});
    }
    return o;
}

QJsonObject timelineToJson(const TimelineData& data, bool active)
{
    int clips = 0;
    for (TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle}) {
        for (const TrackData& t : data.tracks(type))
            clips += int(t.clips.size());
    }
    const int64_t duration = data.duration();
    return {{QStringLiteral("id"), qint64(data.uid)}, {QStringLiteral("name"), data.name},
        {QStringLiteral("active"), active}, {QStringLiteral("fps"), data.fps.toDouble()},
        {QStringLiteral("width"), data.width}, {QStringLiteral("height"), data.height},
        {QStringLiteral("videoTracks"), int(data.videoTracks.size())},
        {QStringLiteral("audioTracks"), int(data.audioTracks.size())},
        {QStringLiteral("clips"), clips}, {QStringLiteral("durationFrames"), qint64(duration)},
        {QStringLiteral("durationTimecode"), timecode::format(duration, data.fps)},
        {QStringLiteral("markers"), int(data.markers.size())}};
}

std::optional<CommandResult> readTime(
    const CommandArgs& args, const QString& name, Rational fps, std::optional<int64_t>& out)
{
    QString error;
    const std::optional<int64_t> value = args.time(name, fps, &error);
    if (args.has(name) && !value)
        return fail("bad_time", error);
    if (value)
        out = value;
    return std::nullopt;
}

std::optional<CommandResult> findClipOrFail(const TimelineData& data, quint64 id, FoundClip& found)
{
    const auto location = editops::findClip(data, id);
    if (!location)
        return fail("unknown_clip", QStringLiteral("no clip with id %1 in this timeline").arg(id));
    found.track = location->track;
    found.clip = &data.track(location->track)->clips[size_t(location->clipIndex)];
    return std::nullopt;
}

Rational rationalFromFps(double fps)
{
    // NTSC rates are n * 1000 / 1001: 23.976, 29.97, 47.952, 59.94, 119.88.
    for (int n : {24, 30, 48, 60, 120}) {
        if (std::abs(fps - n * 1000.0 / 1001.0) < 0.005)
            return Rational {n * 1000, 1001};
    }
    if (std::abs(fps - std::round(fps)) < 1e-9)
        return Rational {int(std::round(fps)), 1};
    return Rational {int(std::llround(fps * 1000.0)), 1000};
}

} // namespace kk::commands
