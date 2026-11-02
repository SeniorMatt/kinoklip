// SPDX-License-Identifier: GPL-3.0-or-later
// Multicam — multicam timeline construction, angle views, grid, angle edits, timecode maths.
#include "core/Multicam.h"

#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Retime.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace kk::multicam {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::multicam", text);
}

void setError(QString* error, const QString& text)
{
    if (error)
        *error = text;
}

/// Whole frames needed to show @p us microseconds at @p fps (rounded up, at least 1).
int64_t framesCovering(int64_t us, Rational fps)
{
    const double frames = double(us) * fps.num / (1e6 * fps.den);
    return std::max<int64_t>(1, int64_t(std::ceil(frames - 1e-6)));
}

const TimelineData* multicamOf(const ClipData& clip, const compound::TimelineLookup& lookup)
{
    if (clip.kind != ClipKind::Nested || !lookup)
        return nullptr;
    const TimelineData* t = lookup(clip.nestedUid);
    return t && t->multicam ? t : nullptr;
}

bool onLockedTrack(const TimelineData& data, quint64 clipId)
{
    const auto loc = editops::findClip(data, clipId);
    return !loc || data.track(loc->track)->locked;
}

} // namespace

int angleCount(const TimelineData& mc)
{
    return int(std::max(mc.videoTracks.size(), mc.audioTracks.size()));
}

QString angleName(const TimelineData& mc, int angle)
{
    if (angle >= 0 && size_t(angle) < mc.videoTracks.size()
        && !mc.videoTracks[size_t(angle)].name.isEmpty())
        return mc.videoTracks[size_t(angle)].name;
    if (angle >= 0 && size_t(angle) < mc.audioTracks.size()
        && !mc.audioTracks[size_t(angle)].name.isEmpty())
        return mc.audioTracks[size_t(angle)].name;
    return tr("Angle %1").arg(angle + 1);
}

TimelineData angleView(const TimelineData& mc, int angle)
{
    // Copy everything but the tracks, then the one angle's tracks (cheap: one or two clips).
    TimelineData view;
    view.uid = mc.uid;
    view.compound = mc.compound;
    view.multicam = mc.multicam;
    view.useProjectSettings = mc.useProjectSettings;
    view.name = mc.name;
    view.fps = mc.fps;
    view.width = mc.width;
    view.height = mc.height;
    view.retimeProcess = mc.retimeProcess;
    view.nextId = mc.nextId;
    if (angle >= 0 && size_t(angle) < mc.videoTracks.size())
        view.videoTracks.push_back(mc.videoTracks[size_t(angle)]);
    if (angle >= 0 && size_t(angle) < mc.audioTracks.size())
        view.audioTracks.push_back(mc.audioTracks[size_t(angle)]);
    return view;
}

QString defaultAngleName(const QString& itemName)
{
    // "CamA_0012.mov" -> "CamA_0012"; names without a short extension stay as they are.
    const qsizetype dot = itemName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && itemName.size() - dot - 1 <= 4 && itemName.size() - dot - 1 >= 2)
        return itemName.left(dot);
    return itemName;
}

std::optional<TimelineData> build(const QVector<AngleSource>& sources, quint64 uid,
    const QString& name, Rational fps, int width, int height, QString* error)
{
    if (sources.isEmpty()) {
        setError(error, tr("Select the clips to put into the multicam clip."));
        return std::nullopt;
    }
    if (sources.size() > kMaxAngles) {
        setError(error, tr("A multicam clip can have at most %1 angles.").arg(kMaxAngles));
        return std::nullopt;
    }
    if (!fps.isValid()) {
        setError(error, tr("The multicam clip needs a frame rate."));
        return std::nullopt;
    }
    for (const AngleSource& s : sources) {
        if (s.info.isStillImage || s.info.durationUs <= 0
            || (!s.info.hasVideo && !s.info.hasAudio)) {
            setError(error, tr("\"%1\" has no duration (stills cannot be angles).").arg(s.name));
            return std::nullopt;
        }
    }
    TimelineData mc;
    mc.uid = uid;
    mc.name = name;
    mc.compound = true;
    mc.multicam = true;
    mc.fps = fps;
    mc.width = std::max(16, width);
    mc.height = std::max(16, height);
    const int64_t origin = std::min_element(
        sources.cbegin(), sources.cend(), [](const AngleSource& a, const AngleSource& b) {
            return a.offsetUs < b.offsetUs;
        })->offsetUs;
    for (const AngleSource& s : sources) {
        TrackData v;
        v.id = mc.allocateId();
        v.type = TrackType::Video;
        v.name = s.name;
        TrackData a;
        a.id = mc.allocateId();
        a.type = TrackType::Audio;
        a.name = s.name;
        // Start on the first whole frame at or after the source's sync position; the
        // sub-frame rest is skipped at the head, so the picture of frame `start` is the
        // media time that belongs there.
        const int64_t delta = s.offsetUs - origin;
        int64_t start = usToFrames(delta, fps);
        if (framesToUs(start, fps) < delta)
            ++start;
        const int64_t headUs = std::max<int64_t>(0, framesToUs(start, fps) - delta);
        if (headUs < s.info.durationUs) {
            ClipData c;
            c.kind = ClipKind::Media;
            c.mediaId = s.mediaId;
            c.name = s.name;
            c.start = start;
            c.sourceInUs = headUs;
            c.sourceDurationUs = s.info.durationUs;
            c.duration = framesCovering(s.info.durationUs - headUs, fps);
            c.linkGroup = s.info.hasVideo && s.info.hasAudio ? mc.allocateId() : 0;
            if (s.info.hasVideo) {
                c.id = mc.allocateId();
                v.clips.push_back(c);
            }
            if (s.info.hasAudio) {
                c.id = mc.allocateId();
                a.clips.push_back(c);
            }
        }
        mc.videoTracks.push_back(std::move(v));
        mc.audioTracks.push_back(std::move(a));
    }
    return mc;
}

ClipData makeMulticamClip(const TimelineData& mc, const QString& mediaId, int angle)
{
    ClipData c = compound::makeNestedClip(mc, mediaId);
    c.multicamAngle = std::clamp(angle, 0, std::max(0, angleCount(mc) - 1));
    return c;
}

bool isMulticamClip(const ClipData& clip, const compound::TimelineLookup& lookup)
{
    return multicamOf(clip, lookup) != nullptr;
}

quint64 multicamClipAt(const TimelineData& data, int64_t frame,
    const compound::TimelineLookup& lookup, bool preferAudio)
{
    auto video = [&]() -> quint64 {
        for (auto it = data.videoTracks.crbegin(); it != data.videoTracks.crend(); ++it) {
            if (!it->enabled)
                continue;
            const ClipData* c = it->clipAt(frame);
            if (c && isMulticamClip(*c, lookup))
                return c->id;
        }
        return 0;
    };
    auto audio = [&]() -> quint64 {
        for (const TrackData& t : data.audioTracks) {
            const ClipData* c = t.clipAt(frame);
            if (c && isMulticamClip(*c, lookup))
                return c->id;
        }
        return 0;
    };
    if (preferAudio) {
        const quint64 a = audio();
        return a ? a : video();
    }
    const quint64 v = video();
    return v ? v : audio();
}

MulticamParts parts(const TimelineData& data, quint64 clipId)
{
    MulticamParts result;
    const ClipData* clip = editops::clipById(data, clipId);
    if (!clip || clip->kind != ClipKind::Nested)
        return result;
    for (const quint64 id : editops::linkedClips(data, clipId)) {
        const auto loc = editops::findClip(data, id);
        if (!loc)
            continue;
        const ClipData& c = data.track(loc->track)->clips[size_t(loc->clipIndex)];
        if (c.kind != ClipKind::Nested || c.nestedUid != clip->nestedUid)
            continue;
        (loc->track.type == TrackType::Video ? result.video : result.audio).append(id);
    }
    return result;
}

bool switchAngle(TimelineData& data, quint64 clipId, int angle, AngleTarget target,
    const compound::TimelineLookup& lookup)
{
    const ClipData* clip = editops::clipById(data, clipId);
    const TimelineData* mc = clip ? multicamOf(*clip, lookup) : nullptr;
    if (!mc || angle < 0 || angle >= angleCount(*mc))
        return false;
    const MulticamParts p = parts(data, clipId);
    QVector<quint64> ids;
    if (target != AngleTarget::Audio)
        ids += p.video;
    if (target != AngleTarget::Video)
        ids += p.audio;
    bool changed = false;
    for (const quint64 id : std::as_const(ids)) {
        if (onLockedTrack(data, id))
            return false;
        changed = changed || editops::clipById(data, id)->multicamAngle != angle;
    }
    if (!changed)
        return false;
    for (const quint64 id : std::as_const(ids))
        editops::clipById(data, id)->multicamAngle = angle;
    return true;
}

QVector<quint64> cutToAngle(TimelineData& data, int64_t frame, int angle, AngleTarget target,
    const compound::TimelineLookup& lookup)
{
    const quint64 clipId = multicamClipAt(data, frame, lookup, target == AngleTarget::Audio);
    const ClipData* clip = clipId ? editops::clipById(data, clipId) : nullptr;
    const TimelineData* mc = clip ? multicamOf(*clip, lookup) : nullptr;
    if (!mc || angle < 0 || angle >= angleCount(*mc))
        return {};
    // Both sides of the segment are cut (the V and A parts stay pairwise linked, like the
    // through edit Resolve leaves on the side that keeps its angle); only the target side
    // switches.
    const MulticamParts p = parts(data, clipId);
    QVector<quint64> group;
    QSet<quint64> targetSide;
    for (const quint64 id : p.video + p.audio) {
        const ClipData* c = editops::clipById(data, id);
        if (!c->contains(frame))
            continue;
        if (onLockedTrack(data, id))
            return {};
        group.append(id);
        const bool video = p.video.contains(id);
        if ((video && target != AngleTarget::Audio) || (!video && target != AngleTarget::Video))
            targetSide.insert(id);
    }
    const bool needed = std::any_of(targetSide.cbegin(), targetSide.cend(),
        [&](quint64 id) { return editops::clipById(data, id)->multicamAngle != angle; });
    if (!needed)
        return {};
    QVector<quint64> result;
    QVector<quint64> toSplit;
    for (const quint64 id : std::as_const(group)) {
        if (editops::clipById(data, id)->start < frame)
            toSplit.append(id);
    }
    // splitClips() returns the right parts in the order of the clips it split.
    const QVector<quint64> rights = editops::splitClips(data, toSplit, frame);
    QHash<quint64, quint64> rightOf;
    for (qsizetype i = 0; i < rights.size() && i < toSplit.size(); ++i)
        rightOf.insert(toSplit[i], rights[i]);
    for (const quint64 id : std::as_const(group)) {
        if (!targetSide.contains(id))
            continue;
        const quint64 shown = rightOf.value(id, id);
        editops::clipById(data, shown)->multicamAngle = angle;
        result.append(shown);
    }
    return result;
}

std::optional<QVector<quint64>> flatten(TimelineData& data, const QVector<quint64>& clipIds,
    const compound::TimelineLookup& lookup, QString* error)
{
    TimelineData out = data;
    QVector<quint64> restored;
    QSet<quint64> done;
    for (const quint64 id : clipIds) {
        if (done.contains(id))
            continue;
        const ClipData* clip = editops::clipById(out, id);
        const TimelineData* mc = clip ? multicamOf(*clip, lookup) : nullptr;
        if (!mc) {
            setError(error, tr("Select a multicam clip to flatten."));
            return std::nullopt;
        }
        const MulticamParts p = parts(out, id);
        for (const quint64 part : p.video + p.audio)
            done.insert(part);
        // Parts showing one angle go in one decompose() call, which keeps their V/A links.
        QHash<int, QVector<quint64>> byAngle;
        for (const quint64 part : p.video + p.audio)
            byAngle[std::max(0, editops::clipById(out, part)->multicamAngle)].append(part);
        for (auto it = byAngle.cbegin(); it != byAngle.cend(); ++it) {
            const std::optional<QVector<quint64>> r
                = compound::decompose(out, it.value(), angleView(*mc, it.key()), error);
            if (!r)
                return std::nullopt;
            restored += *r;
        }
    }
    data = std::move(out);
    return restored;
}

int columnsFor(int angles)
{
    return angles <= 4 ? 2 : angles <= 9 ? 3 : 4;
}

int angleAtCell(int angles, int columns, int page, int column, int row)
{
    if (columns < 1 || column < 0 || row < 0 || column >= columns || row >= columns)
        return -1;
    const int angle = page * columns * columns + row * columns + column;
    return angle < angles ? angle : -1;
}

TimelineData gridTimeline(
    const TimelineData& mc, int columns, int page, int audioAngle, const QString& mediaId)
{
    columns = std::clamp(columns, 1, 4);
    TimelineData grid;
    grid.name = mc.name;
    grid.fps = mc.fps;
    grid.width = mc.width;
    grid.height = mc.height;
    grid.markIn = -1;
    grid.markOut = -1;
    const int angles = angleCount(mc);
    const ClipData base = makeMulticamClip(mc, mediaId, 0);
    const double cellW = double(mc.width) / columns;
    const double cellH = double(mc.height) / columns;
    for (int row = 0; row < columns; ++row) {
        for (int column = 0; column < columns; ++column) {
            const int angle = angleAtCell(angles, columns, page, column, row);
            if (angle < 0)
                continue;
            TrackData t;
            t.id = grid.allocateId();
            t.type = TrackType::Video;
            t.name = angleName(mc, angle);
            ClipData c = base;
            c.id = grid.allocateId();
            c.multicamAngle = angle;
            c.transform.setBase(keys::ZoomX, 1.0 / columns);
            c.transform.setBase(keys::ZoomY, 1.0 / columns);
            c.transform.setBase(keys::PosX, (column + 0.5) * cellW - mc.width / 2.0);
            c.transform.setBase(keys::PosY, (row + 0.5) * cellH - mc.height / 2.0);
            t.clips.push_back(std::move(c));
            grid.videoTracks.push_back(std::move(t));
        }
    }
    TrackData a;
    a.id = grid.allocateId();
    a.type = TrackType::Audio;
    if (audioAngle >= 0 && audioAngle < angles) {
        ClipData c = base;
        c.id = grid.allocateId();
        c.multicamAngle = audioAngle;
        a.clips.push_back(std::move(c));
    }
    grid.audioTracks.push_back(std::move(a));
    return grid;
}

// ---- Sync offsets ----

std::optional<int64_t> timecodeToUs(const QString& timecode, Rational fps)
{
    if (!fps.isValid())
        return std::nullopt;
    static const QRegularExpression re(
        QStringLiteral("^\\s*(\\d{1,2})[:;.](\\d{2})[:;.](\\d{2})([:;.])(\\d{2,3})\\s*$"));
    const QRegularExpressionMatch m = re.match(timecode);
    if (!m.hasMatch())
        return std::nullopt;
    const int64_t h = m.captured(1).toLongLong();
    const int64_t min = m.captured(2).toLongLong();
    const int64_t s = m.captured(3).toLongLong();
    const int64_t f = m.captured(5).toLongLong();
    const auto nominal = int64_t(std::llround(fps.toDouble()));
    if (nominal < 1 || min > 59 || s > 59 || f >= nominal)
        return std::nullopt;
    int64_t frames = ((h * 60 + min) * 60 + s) * nominal + f;
    const bool dropFrame = timecode.contains(QLatin1Char(';'));
    // Drop frame (29.97 / 59.94 only): frame numbers 0, 1 (x2 at 59.94) of every minute are
    // skipped except in minutes divisible by ten, so the count matches real time.
    if (dropFrame && fps.den == 1001 && (nominal == 30 || nominal == 60)) {
        const int64_t dropPerMinute = nominal / 15;
        const int64_t minutes = h * 60 + min;
        frames -= dropPerMinute * (minutes - minutes / 10);
    }
    return framesToUs(frames, fps);
}

QVector<std::optional<int64_t>> offsetsFromTimecode(const QVector<media::MediaInfo>& infos)
{
    QVector<std::optional<int64_t>> result;
    result.reserve(infos.size());
    for (const media::MediaInfo& info : infos) {
        // Audio-only files have no frame rate: their timecode counts at 25 frames.
        const Rational rate = info.frameRate.isValid() ? info.frameRate : Rational {25, 1};
        result.append(timecodeToUs(info.startTimecode, rate));
    }
    return result;
}

QVector<int64_t> offsetsFromInPoints(const QVector<int64_t>& inPointsUs)
{
    QVector<int64_t> result;
    result.reserve(inPointsUs.size());
    for (const int64_t in : inPointsUs)
        result.append(-std::max<int64_t>(0, in));
    return result;
}

} // namespace kk::multicam
