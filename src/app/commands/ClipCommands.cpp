// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: clip.list, clip.add, clip.move, clip.trim, clip.split, clip.delete, clip.set.
// Every change goes through editops (the functions the timeline uses) inside one undo step.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/EditOps.h"
#include "core/LevelOps.h"
#include "core/MediaPool.h"
#include "core/MixerData.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <QFileInfo>

namespace kk::commands {

namespace ops = editops;
using timelineedit::runEdit;

namespace {

constexpr double kMinSpeed = 0.01; ///< slowest and fastest constant speed accepted (1 % - 100x)
constexpr double kMaxSpeed = 100.0;
constexpr double kMinZoom = 0.001; ///< smallest scale factor
constexpr double kMaxVolumeDb = 24.0;
constexpr double kMinVolumeDb = -96.0; ///< silence, like the inspector
constexpr double kMaxCrop = 10000; ///< pixels, like the Cropping group of the inspector

const TimelineData& dataOf(CommandContext& ctx)
{
    return ctx.project->timeline()->data();
}

const ParamSpec kUnlinked {QStringLiteral("unlinked"), ParamType::Boolean,
    QStringLiteral(
        "change only this clip, not the clips linked to it (video + audio of one source)"),
    false, {}, {}};
const ParamSpec kClipParam {QStringLiteral("clip"), ParamType::Integer,
    QStringLiteral("clip id (see clip.list)"), true, {}, {}};

/// @p ids plus, unless @p unlinked, the clips linked to them.
QVector<quint64> withLinks(const TimelineData& data, const QVector<quint64>& ids, bool unlinked)
{
    return unlinked ? ids : ops::withLinkedClips(data, ids);
}

QJsonObject clipById(CommandContext& ctx, quint64 id)
{
    const TimelineData& d = dataOf(ctx);
    FoundClip found;
    if (findClipOrFail(d, id, found))
        return {};
    return clipToJson(d, found.track, *found.clip);
}

CommandResult runList(CommandContext& ctx, const CommandArgs& args)
{
    const TimelineData& d = dataOf(ctx);
    std::optional<TrackRef> only;
    if (args.has(QStringLiteral("track"))) {
        only = parseTrack(d, args.string(QStringLiteral("track")));
        if (!only)
            return fail("unknown_track",
                QStringLiteral("no track \"%1\"").arg(args.string(QStringLiteral("track"))));
    }
    QJsonArray clips;
    QString text;
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        const auto& tracks = d.tracks(type);
        for (int i = 0; i < int(tracks.size()); ++i) {
            const TrackRef ref {type, i};
            if (only && !(*only == ref))
                continue;
            for (const ClipData& c : tracks[size_t(i)].clips) {
                const QJsonObject o = clipToJson(d, ref, c);
                clips.append(o);
                text += QStringLiteral("%1  %2  %3  %4  %5\n")
                            .arg(c.id)
                            .arg(trackName(ref), o[QStringLiteral("startTimecode")].toString(),
                                o[QStringLiteral("durationTimecode")].toString(), c.name);
            }
        }
    }
    return CommandResult::success(
        {{QStringLiteral("timeline"), qint64(d.uid)}, {QStringLiteral("fps"), d.fps.toDouble()},
            {QStringLiteral("clips"), clips}},
        text.isEmpty() ? QStringLiteral("no clips") : text.trimmed());
}

/// The media pool item named by @p key: an id, else a unique name, else a file path.
const MediaItem* findMedia(const MediaPool& pool, const QString& key)
{
    if (const MediaItem* item = pool.item(key))
        return item;
    const MediaItem* byName = nullptr;
    int matches = 0;
    for (const MediaItem& m : pool.items()) {
        if (m.name == key) {
            byName = &m;
            ++matches;
        }
    }
    if (matches == 1)
        return byName;
    return pool.itemByPath(QFileInfo(key).absoluteFilePath());
}

CommandResult runAdd(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = dataOf(ctx);
    const QString key = args.string(QStringLiteral("media"));
    const MediaItem* item = findMedia(*project.mediaPool(), key);
    if (!item)
        return fail(
            "unknown_media", QStringLiteral("no media with id, name or path \"%1\"").arg(key));
    if (project.mediaPool()->isOffline(item->id))
        return fail(
            "media_offline", QStringLiteral("%1 is offline (media.relink)").arg(item->name));
    const QString trackText = args.string(QStringLiteral("track"), QStringLiteral("V1"));
    const std::optional<TrackRef> track = parseTrack(d, trackText);
    if (!track)
        return fail(
            "unknown_track", QStringLiteral("no track \"%1\" in this timeline").arg(trackText));

    // A video track gets the picture and, if the media has sound, the audio track with the same
    // number (else A1); an audio track gets the sound only.
    int videoTrack = -1;
    int audioTrack = -1;
    if (track->type == TrackType::Video) {
        videoTrack = track->index;
        if (!d.audioTracks.empty())
            audioTrack = track->index < int(d.audioTracks.size()) ? track->index : 0;
    } else {
        audioTrack = track->index;
    }

    std::optional<int64_t> sourceIn;
    std::optional<int64_t> sourceOut;
    std::optional<int64_t> at;
    if (auto bad = readTime(args, QStringLiteral("source-in"), d.fps, sourceIn))
        return *bad;
    if (auto bad = readTime(args, QStringLiteral("source-out"), d.fps, sourceOut))
        return *bad;
    if (auto bad = readTime(args, QStringLiteral("at"), d.fps, at))
        return *bad;
    const int64_t inUs = framesToUs(sourceIn.value_or(0), d.fps);
    const int64_t outUs = sourceOut ? framesToUs(*sourceOut, d.fps) : 0;
    if (inUs < 0 || (sourceOut && outUs <= inUs))
        return fail("out_of_range", QStringLiteral("source-in must be >= 0 and before source-out"));
    if (item->info.durationUs > 0 && inUs >= item->info.durationUs)
        return fail(
            "out_of_range", QStringLiteral("source-in is past the end of %1").arg(item->name));
    if (at && *at < 0)
        return fail(
            "out_of_range", QStringLiteral("the position is before the start of the timeline"));

    SelectionModel selection;
    AppContext appContext;
    appContext.project = &project;
    appContext.selection = &selection;
    TimelineController controller(appContext);
    QVector<ops::PlacedClip> clips
        = controller.makeMediaClips(item->id, inUs, outUs, videoTrack, audioTrack);
    if (clips.isEmpty())
        return fail("no_content",
            QStringLiteral("%1 has nothing to place on %2 (or would contain this timeline)")
                .arg(item->name, trackText));
    for (const ops::PlacedClip& p : std::as_const(clips)) {
        if (d.track(p.track)->locked)
            return fail(
                "track_locked", QStringLiteral("track %1 is locked").arg(trackName(p.track)));
    }
    int64_t start = at.value_or(0);
    if (!at) {
        for (const ops::PlacedClip& p : std::as_const(clips))
            start = std::max(start, d.track(p.track)->end());
    }
    for (ops::PlacedClip& p : clips)
        p.clip.start = start;

    QVector<quint64> ids;
    const bool insert = args.boolean(QStringLiteral("insert"));
    const bool changed = runEdit(&project, QStringLiteral("Add Clip"), [&](TimelineData& data) {
        ids = insert ? ops::insertClips(data, clips, ops::RippleScope::AllTracks)
                     : ops::overwriteClips(data, clips);
        if (ids.size() > 1)
            ops::linkClips(data, ids); // video + audio of one source stay in sync
        return !ids.isEmpty();
    });
    if (!changed)
        return fail("edit_refused", QStringLiteral("the clip could not be placed"));
    QJsonArray added;
    for (quint64 id : std::as_const(ids))
        added.append(clipById(ctx, id));
    return CommandResult::success({{QStringLiteral("clips"), added}},
        QStringLiteral("added %1 clip(s) at %2: ids %3")
            .arg(ids.size())
            .arg(timecode::format(start, d.fps))
            .arg([&] {
                QStringList s;
                for (quint64 id : std::as_const(ids))
                    s << QString::number(id);
                return s.join(QStringLiteral(", "));
            }()));
}

CommandResult runMove(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = dataOf(ctx);
    const quint64 id = quint64(args.integer(QStringLiteral("clip")));
    FoundClip found;
    if (auto bad = findClipOrFail(d, id, found))
        return *bad;
    std::optional<int64_t> to;
    if (auto bad = readTime(args, QStringLiteral("to"), d.fps, to))
        return *bad;
    TrackRef target = found.track;
    if (args.has(QStringLiteral("track"))) {
        const auto parsed = parseTrack(d, args.string(QStringLiteral("track")));
        if (!parsed)
            return fail("unknown_track",
                QStringLiteral("no track \"%1\"").arg(args.string(QStringLiteral("track"))));
        if (parsed->type != found.track.type)
            return fail(
                "bad_value", QStringLiteral("a clip stays on its kind of track (video or audio)"));
        target = *parsed;
    }
    if (!to && target == found.track)
        return fail("missing_parameter", QStringLiteral("give --to and/or --track"));
    const int64_t newStart = to.value_or(found.clip->start);
    if (newStart < 0)
        return fail(
            "out_of_range", QStringLiteral("the position is before the start of the timeline"));
    const int64_t delta = newStart - found.clip->start;
    const int deltaTrack = target.index - found.track.index;
    if (delta == 0 && deltaTrack == 0)
        return fail("nothing_to_do", QStringLiteral("the clip is already there"));
    QVector<quint64> partners = withLinks(d, {id}, args.boolean(QStringLiteral("unlinked")));
    partners.removeAll(id);

    const bool changed = runEdit(&project, QStringLiteral("Move Clip"), [&](TimelineData& data) {
        if (!ops::moveClips(data, {id}, delta, deltaTrack))
            return false;
        if (delta != 0 && !partners.isEmpty())
            ops::moveClips(data, partners, delta, 0);
        return true;
    });
    if (!changed)
        return fail("edit_refused", QStringLiteral("the clip could not be moved (locked track?)"));
    return CommandResult::success({{QStringLiteral("clip"), clipById(ctx, id)}},
        QStringLiteral("moved clip %1 to %2 at %3")
            .arg(id)
            .arg(trackName(target), timecode::format(newStart, d.fps)));
}

CommandResult runTrim(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = dataOf(ctx);
    const quint64 id = quint64(args.integer(QStringLiteral("clip")));
    FoundClip found;
    if (auto bad = findClipOrFail(d, id, found))
        return *bad;
    const bool head = args.string(QStringLiteral("edge")) == QLatin1String("head");
    const bool ripple = args.boolean(QStringLiteral("ripple"));
    std::optional<int64_t> requested;
    if (auto bad = readTime(args, QStringLiteral("delta"), d.fps, requested))
        return *bad;
    if (!requested || *requested == 0)
        return fail("bad_value", QStringLiteral("delta must be a non-zero number of frames"));

    // Linked clips with an edge on the same frame trim along.
    QVector<quint64> partners;
    if (!args.boolean(QStringLiteral("unlinked"))) {
        const int64_t edge = head ? found.clip->start : found.clip->end();
        for (quint64 other : ops::linkedClips(d, id)) {
            const ClipData* c = ops::clipById(d, other);
            if (other != id && c && (head ? c->start : c->end()) == edge)
                partners.append(other);
        }
    }
    int64_t applied = 0;
    runEdit(&project, QStringLiteral("Trim Clip"), [&](TimelineData& data) {
        if (ripple) {
            QVector<quint64> group {id};
            group += partners;
            applied = ops::rippleTrimSynced(data, group, head, *requested);
            return applied != 0;
        }
        applied = head ? ops::trimHead(data, id, *requested, ripple)
                       : ops::trimTail(data, id, *requested, ripple);
        if (applied == 0)
            return false;
        for (quint64 other : std::as_const(partners)) {
            if (head)
                ops::trimHead(data, other, applied, ripple);
            else
                ops::trimTail(data, other, applied, ripple);
        }
        return true;
    });
    if (applied == 0)
        return fail("out_of_range",
            QStringLiteral("the edge cannot move that way (end of the media, a neighbouring clip, "
                           "minimum length of one frame or a locked track)"));
    return CommandResult::success(
        {{QStringLiteral("clip"), clipById(ctx, id)}, {QStringLiteral("applied"), qint64(applied)},
            {QStringLiteral("clamped"), applied != *requested}},
        QStringLiteral("trimmed the %1 of clip %2 by %3 frame(s)%4")
            .arg(head ? QStringLiteral("head") : QStringLiteral("tail"))
            .arg(id)
            .arg(applied)
            .arg(applied != *requested ? QStringLiteral(" (limited)") : QString()));
}

CommandResult runSplit(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = dataOf(ctx);
    std::optional<int64_t> frame;
    if (auto bad = readTime(args, QStringLiteral("at"), d.fps, frame))
        return *bad;
    QVector<quint64> created;
    if (args.has(QStringLiteral("clip"))) {
        const quint64 id = quint64(args.integer(QStringLiteral("clip")));
        FoundClip found;
        if (auto bad = findClipOrFail(d, id, found))
            return *bad;
        if (*frame <= found.clip->start || *frame >= found.clip->end())
            return fail("out_of_range",
                QStringLiteral("clip %1 covers frames %2 to %3; split strictly inside it")
                    .arg(id)
                    .arg(found.clip->start)
                    .arg(found.clip->end() - 1));
        const QVector<quint64> ids = withLinks(d, {id}, args.boolean(QStringLiteral("unlinked")));
        runEdit(&project, QStringLiteral("Split Clip"), [&](TimelineData& data) {
            created = ops::splitClips(data, ids, *frame);
            return !created.isEmpty();
        });
    } else {
        runEdit(&project, QStringLiteral("Split Clip"), [&](TimelineData& data) {
            created = ops::splitAt(data, *frame, ops::autoSelectTracks());
            return !created.isEmpty();
        });
    }
    if (created.isEmpty())
        return fail("out_of_range", QStringLiteral("no clip to split at frame %1").arg(*frame));
    QJsonArray added;
    for (quint64 id : std::as_const(created))
        added.append(clipById(ctx, id));
    return CommandResult::success({{QStringLiteral("clips"), added}},
        QStringLiteral("split at %1: %2 new clip(s)")
            .arg(timecode::format(*frame, d.fps))
            .arg(created.size()));
}

CommandResult runDelete(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = dataOf(ctx);
    QString error;
    const QVector<quint64> requested = parseIds(args.strings(QStringLiteral("clip")), &error);
    if (!error.isEmpty())
        return fail("bad_value", error);
    if (requested.isEmpty())
        return fail("missing_parameter", QStringLiteral("no clip ids given"));
    for (quint64 id : requested) {
        FoundClip found;
        if (auto bad = findClipOrFail(d, id, found))
            return *bad;
    }
    const QVector<quint64> ids = withLinks(d, requested, args.boolean(QStringLiteral("unlinked")));
    const bool ripple = args.boolean(QStringLiteral("ripple"));
    int removed = 0;
    runEdit(&project, ripple ? QStringLiteral("Ripple Delete") : QStringLiteral("Delete Clip"),
        [&](TimelineData& data) {
            removed = ripple ? ops::rippleDeleteClips(data, ids, ops::RippleScope::AllTracks)
                             : ops::liftClips(data, ids);
            return removed > 0;
        });
    if (removed == 0)
        return fail("edit_refused", QStringLiteral("nothing was deleted (locked track?)"));
    return CommandResult::success({{QStringLiteral("deleted"), removed}},
        QStringLiteral("deleted %1 clip(s)%2")
            .arg(removed)
            .arg(ripple ? QStringLiteral(" (ripple)") : QString()));
}

CommandResult runSet(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = dataOf(ctx);
    const quint64 id = quint64(args.integer(QStringLiteral("clip")));
    FoundClip found;
    if (auto bad = findClipOrFail(d, id, found))
        return *bad;
    if (d.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));

    // Transform values, by option name.
    struct Setting
    {
        const char* option;
        const QString* key;
        double min;
        double max;
    };
    static const double kBig = 1e6;
    static const Setting numeric[]
        = {{"pos-x", &keys::PosX, -kBig, kBig}, {"pos-y", &keys::PosY, -kBig, kBig},
            {"rotation", &keys::Rotation, -kBig, kBig}, {"opacity", &keys::Opacity, 0, 100},
            {"crop-left", &keys::CropL, 0, kMaxCrop}, {"crop-right", &keys::CropR, 0, kMaxCrop},
            {"crop-top", &keys::CropT, 0, kMaxCrop}, {"crop-bottom", &keys::CropB, 0, kMaxCrop}};
    bool any = false;
    for (const char* name :
        {"speed", "volume", "enabled", "name", "zoom", "pos-x", "pos-y", "rotation", "opacity",
            "crop-left", "crop-right", "crop-top", "crop-bottom", "pan", "fade-in", "fade-out",
            "fade-in-curve", "fade-out-curve", "stabilization", "pitch", "eq"})
        any |= args.has(QString::fromLatin1(name));
    for (int b = 1; b <= 4; ++b)
        for (const char* part : {"freq", "gain", "q"})
            any |= args.has(QStringLiteral("eq%1-%2").arg(b).arg(QLatin1String(part)));
    if (!any)
        return fail(
            "missing_parameter", QStringLiteral("nothing to set: give at least one option"));
    auto range = [&](const char* option, double value, double min, double max) {
        if (value < min || value > max)
            return fail("out_of_range",
                QStringLiteral("--%1 must be between %2 and %3")
                    .arg(QString::fromLatin1(option))
                    .arg(min)
                    .arg(max));
        return CommandResult::success({}, {});
    };
    if (args.has(QStringLiteral("speed"))) {
        if (auto r = range("speed", args.number(QStringLiteral("speed")), kMinSpeed, kMaxSpeed);
            !r.ok)
            return r;
    }
    if (args.has(QStringLiteral("volume"))) {
        if (auto r
            = range("volume", args.number(QStringLiteral("volume")), kMinVolumeDb, kMaxVolumeDb);
            !r.ok)
            return r;
    }
    if (args.has(QStringLiteral("pan"))) {
        if (auto r = range("pan", args.number(QStringLiteral("pan")), -1.0, 1.0); !r.ok)
            return r;
    }
    if (args.has(QStringLiteral("pitch"))) {
        if (auto r = range("pitch", args.number(QStringLiteral("pitch")), -24.0, 24.0); !r.ok)
            return r;
    }
    if (args.has(QStringLiteral("eq"))) {
        const QString eq = args.string(QStringLiteral("eq"));
        if (eq != QLatin1String("on") && eq != QLatin1String("off"))
            return fail("bad_value", QStringLiteral("--eq must be \"on\" or \"off\""));
    }
    for (int b = 1; b <= 4; ++b) {
        const QString o = QStringLiteral("eq%1-").arg(b);
        for (const auto& [part, lo, hi] :
            {std::tuple {QStringLiteral("freq"), kEqMinFrequencyHz, kEqMaxFrequencyHz},
                std::tuple {QStringLiteral("gain"), -kEqMaxGainDb, kEqMaxGainDb},
                std::tuple {QStringLiteral("q"), kEqMinQ, kEqMaxQ}}) {
            const QString name = o + part;
            if (args.has(name))
                if (auto r = range(name.toLatin1().constData(), args.number(name), lo, hi); !r.ok)
                    return r;
        }
    }
    if (args.has(QStringLiteral("zoom"))) {
        if (auto r = range("zoom", args.number(QStringLiteral("zoom")), kMinZoom, kBig); !r.ok)
            return r;
    }
    // Fades: lengths are times (frames or timecode), curves -1 (fast start) ... 1 (slow start).
    std::optional<int64_t> fadeLength[2]; // in, out
    const char* const fadeNames[2] = {"fade-in", "fade-out"};
    const char* const curveNames[2] = {"fade-in-curve", "fade-out-curve"};
    for (int k = 0; k < 2; ++k) {
        if (auto bad = readTime(args, QString::fromLatin1(fadeNames[k]), d.fps, fadeLength[k]))
            return *bad;
        if (fadeLength[k] && *fadeLength[k] < 0)
            return fail("out_of_range",
                QStringLiteral("--%1 must not be negative").arg(QString::fromLatin1(fadeNames[k])));
        if (args.has(QString::fromLatin1(curveNames[k]))) {
            if (auto r
                = range(curveNames[k], args.number(QString::fromLatin1(curveNames[k])), -1.0, 1.0);
                !r.ok)
                return r;
            const int64_t length = fadeLength[k]
                ? *fadeLength[k]
                : (k == 0 ? found.clip->fadeInFrames : found.clip->fadeOutFrames);
            if (length <= 0)
                return fail("no_fade",
                    QStringLiteral("--%1 needs a fade: the clip has no %2; give --%2 too")
                        .arg(
                            QString::fromLatin1(curveNames[k]), QString::fromLatin1(fadeNames[k])));
        }
    }
    for (const Setting& s : numeric) {
        if (args.has(QString::fromLatin1(s.option)))
            if (auto r = range(s.option, args.number(QString::fromLatin1(s.option)), s.min, s.max);
                !r.ok)
                return r;
    }

    const QVector<quint64> speedIds = withLinks(d, {id}, args.boolean(QStringLiteral("unlinked")));
    runEdit(&project, QStringLiteral("Set Clip"), [&](TimelineData& data) {
        ClipData* clip = ops::clipById(data, id);
        if (args.has(QStringLiteral("speed"))) {
            for (quint64 other : speedIds)
                timelineedit::applyClipSpeed(data, other, args.number(QStringLiteral("speed")));
            clip = ops::clipById(data, id);
        }
        if (args.has(QStringLiteral("volume")))
            clip->audio.setBase(keys::VolumeDb, args.number(QStringLiteral("volume")));
        if (args.has(QStringLiteral("pan")))
            clip->audio.setBase(keys::Pan, args.number(QStringLiteral("pan")));
        if (args.has(QStringLiteral("pitch"))) {
            const double cents = std::round(args.number(QStringLiteral("pitch")) * 100.0);
            const double semitones = std::trunc(cents / 100.0);
            clip->audio.setBase(keys::PitchSemitones, semitones);
            clip->audio.setBase(keys::PitchCents, cents - semitones * 100.0);
        }
        if (args.has(QStringLiteral("eq")))
            clip->audio.setStatic(
                keys::EqEnabled, args.string(QStringLiteral("eq")) == QLatin1String("on"));
        for (int b = 0; b < 4; ++b) {
            const QString o = QStringLiteral("eq%1-").arg(b + 1);
            if (args.has(o + QLatin1String("freq")))
                clip->audio.setBase(keys::EqBandFreq[b], args.number(o + QLatin1String("freq")));
            if (args.has(o + QLatin1String("gain")))
                clip->audio.setBase(keys::EqBandGain[b], args.number(o + QLatin1String("gain")));
            if (args.has(o + QLatin1String("q")))
                clip->audio.setBase(keys::EqBandQ[b], args.number(o + QLatin1String("q")));
        }
        if (args.has(QStringLiteral("enabled")))
            clip->enabled = args.boolean(QStringLiteral("enabled"));
        if (args.has(QStringLiteral("name")))
            clip->name = args.string(QStringLiteral("name"));
        if (args.has(QStringLiteral("stabilization")))
            clip->stabilization.settings.enabled
                = args.string(QStringLiteral("stabilization")) == QLatin1String("on");
        if (args.has(QStringLiteral("zoom"))) {
            clip->transform.setBase(keys::ZoomX, args.number(QStringLiteral("zoom")));
            clip->transform.setBase(keys::ZoomY, args.number(QStringLiteral("zoom")));
        }
        for (const Setting& s : numeric) {
            if (args.has(QString::fromLatin1(s.option)))
                clip->transform.setBase(*s.key, args.number(QString::fromLatin1(s.option)));
        }
        // Fades act on this clip only, like the fade handles (linked clips keep their own).
        for (int k = 0; k < 2; ++k) {
            if (fadeLength[k])
                levelops::setFade(data, id, k == 0, *fadeLength[k]);
            if (args.has(QString::fromLatin1(curveNames[k])))
                levelops::setFadeCurve(
                    data, id, k == 0, args.number(QString::fromLatin1(curveNames[k])));
        }
        return true;
    });
    CommandResult done = CommandResult::success(
        {{QStringLiteral("clip"), clipById(ctx, id)}}, QStringLiteral("updated clip %1").arg(id));
    FoundClip after;
    if (args.has(QStringLiteral("pitch")) && !findClipOrFail(dataOf(ctx), id, after)
        && retime::isRetimed(*after.clip)) {
        const QString warning = QStringLiteral(
            "pitch is saved but ignored until the clip's speed is normal (no speed change or "
            "retime)");
        done.data.insert(QStringLiteral("warning"), warning);
        done.text += QStringLiteral("\nwarning: ") + warning;
    }
    return done;
}

ParamSpec timeParam(const char* name, const char* description, bool required = false)
{
    return {QString::fromLatin1(name), ParamType::Time, QString::fromLatin1(description), required,
        {}, {}};
}

} // namespace

void registerClipCommands(CommandRegistry& registry)
{
    const QString times = QStringLiteral(" Times are timeline frames or timecode (HH:MM:SS:FF) "
                                         "counted from the start of the timeline.");
    registry.add(
        {{QStringLiteral("clip.list"),
             QStringLiteral(
                 "List the clips of a timeline: id, track, start, duration, source in/out.")
                 + times,
             {{QStringLiteral("track"), ParamType::String,
                 QStringLiteral("only this track (V1, A2, ...)"), false, {}, {}}},
             ProjectUse::Read, true},
            runList});
    registry.add(
        {{QStringLiteral("clip.add"),
             QStringLiteral("Place a media item on a track: appended after the last clip, or at a "
                            "time (overwrites, or inserts and ripples with --insert).")
                 + times,
             {{QStringLiteral("media"), ParamType::String,
                  QStringLiteral("media id, name or file path (see media.list)"), true, {}, {}},
                 {QStringLiteral("track"), ParamType::String,
                     QStringLiteral("target track; a video track also gets the sound on the "
                                    "same-numbered audio track"),
                     false, QStringLiteral("V1"), {}},
                 timeParam("at", "position on the timeline (default: the end of the track)"),
                 {QStringLiteral("insert"), ParamType::Boolean,
                     QStringLiteral("insert and push later clips right instead of overwriting"),
                     false, {}, {}},
                 timeParam("source-in", "first frame of the media to use (default: the start)"),
                 timeParam("source-out", "end of the part of the media to use (default: the end)")},
             ProjectUse::Modify, true},
            runAdd});
    registry.add(
        {{QStringLiteral("clip.move"),
             QStringLiteral(
                 "Move a clip to another time and/or track (anything under it is overwritten).")
                 + times,
             {kClipParam, timeParam("to", "new start of the clip"),
                 {QStringLiteral("track"), ParamType::String,
                     QStringLiteral("new track of the same kind"), false, {}, {}},
                 kUnlinked},
             ProjectUse::Modify, true},
            runMove});
    registry.add(
        {{QStringLiteral("clip.trim"),
             QStringLiteral("Move the head or tail of a clip by a number of frames (positive = "
                            "later; head +n shortens, tail +n lengthens).")
                 + times,
             {kClipParam,
                 {QStringLiteral("edge"), ParamType::String, QStringLiteral("which edge"), true, {},
                     {QStringLiteral("head"), QStringLiteral("tail")}},
                 timeParam("delta", "signed distance to move the edge", true),
                 {QStringLiteral("ripple"), ParamType::Boolean,
                     QStringLiteral("shift later clips on the track instead of leaving a gap"),
                     false, {}, {}},
                 kUnlinked},
             ProjectUse::Modify, true},
            runTrim});
    registry.add({{QStringLiteral("clip.split"),
                      QStringLiteral("Split clips at a time: one clip with --clip, else every clip "
                                     "on the auto-selected tracks.")
                          + times,
                      {timeParam("at", "where to cut", true),
                          {QStringLiteral("clip"), ParamType::Integer,
                              QStringLiteral("only this clip"), false, {}, {}},
                          kUnlinked},
                      ProjectUse::Modify, true},
        runSplit});
    registry.add({{QStringLiteral("clip.delete"),
                      QStringLiteral("Delete clips, leaving a gap, or closing it with --ripple."),
                      {{QStringLiteral("clip"), ParamType::StringList,
                           QStringLiteral("clip id (repeat the option, or separate ids by commas)"),
                           true, {}, {}},
                          {QStringLiteral("ripple"), ParamType::Boolean,
                              QStringLiteral("close the gap on all tracks"), false, {}, {}},
                          kUnlinked},
                      ProjectUse::Modify, true},
        runDelete});
    CommandSpec set {QStringLiteral("clip.set"),
        QStringLiteral("Change properties of a clip: speed, volume, pan, pitch, the clip "
                       "equalizer, enabled, name, basic "
                       "transform, crop, the fades (lengths and curves) and "
                       "stabilization on/off."),
        {kClipParam,
            {QStringLiteral("speed"), ParamType::Number,
                QStringLiteral("constant speed, 1 = normal, 0.5 = half (0.01 - 100); linked "
                               "clips follow"),
                false, {}, {}},
            {QStringLiteral("volume"), ParamType::Number, QStringLiteral("gain in dB (-96 - 24)"),
                false, {}, {}},
            {QStringLiteral("enabled"), ParamType::Boolean,
                QStringLiteral("true or false: switch the clip on or off"), false, {}, {}},
            {QStringLiteral("name"), ParamType::String, QStringLiteral("clip name"), false, {}, {}},
            {QStringLiteral("zoom"), ParamType::Number, QStringLiteral("scale factor, 1 = normal"),
                false, {}, {}},
            {QStringLiteral("pos-x"), ParamType::Number,
                QStringLiteral("horizontal position in pixels from the centre"), false, {}, {}},
            {QStringLiteral("pos-y"), ParamType::Number,
                QStringLiteral("vertical position in pixels from the centre (down is positive)"),
                false, {}, {}},
            {QStringLiteral("rotation"), ParamType::Number, QStringLiteral("degrees clockwise"),
                false, {}, {}},
            {QStringLiteral("opacity"), ParamType::Number, QStringLiteral("percent, 0 - 100"),
                false, {}, {}},
            {QStringLiteral("crop-left"), ParamType::Number,
                QStringLiteral("pixels cut from the left edge of the source (0 - 10000)"), false,
                {}, {}},
            {QStringLiteral("crop-right"), ParamType::Number,
                QStringLiteral("pixels cut from the right edge"), false, {}, {}},
            {QStringLiteral("crop-top"), ParamType::Number,
                QStringLiteral("pixels cut from the top edge"), false, {}, {}},
            {QStringLiteral("crop-bottom"), ParamType::Number,
                QStringLiteral("pixels cut from the bottom edge"), false, {}, {}},
            {QStringLiteral("pan"), ParamType::Number,
                QStringLiteral("stereo pan, -1 (left) to 1 (right)"), false, {}, {}},
            {QStringLiteral("pitch"), ParamType::Number,
                QStringLiteral("pitch in semitones, -24 to 24 (fractions are cents); the "
                               "clip keeps its length"),
                false, {}, {}},
            {QStringLiteral("eq"), ParamType::String,
                QStringLiteral("the clip equalizer, \"on\" or \"off\""), false, {},
                {QStringLiteral("on"), QStringLiteral("off")}},
            {QStringLiteral("fade-in"), ParamType::Time,
                QStringLiteral("fade-in length (frames or timecode, 0 = none); only this clip"),
                false, {}, {}},
            {QStringLiteral("fade-out"), ParamType::Time,
                QStringLiteral("fade-out length (frames or timecode, 0 = none); only this clip"),
                false, {}, {}},
            {QStringLiteral("fade-in-curve"), ParamType::Number,
                QStringLiteral("fade-in shape, -1 (fast start) to 1 (slow start), 0 = linear"),
                false, {}, {}},
            {QStringLiteral("fade-out-curve"), ParamType::Number,
                QStringLiteral("fade-out shape, -1 (fast start) to 1 (slow start), 0 = linear"),
                false, {}, {}},
            {QStringLiteral("stabilization"), ParamType::String,
                QStringLiteral("\"off\" switches the clip's stabilization off (the "
                               "measurement is kept), \"on\" switches it on again; see "
                               "clip.stabilize to measure"),
                false, {}, {QStringLiteral("on"), QStringLiteral("off")}},
            kUnlinked},
        ProjectUse::Modify, true};
    // Equalizer bands: band 1 low shelf, 2 and 3 bells, 4 high shelf.
    for (int b = 1; b <= 4; ++b) {
        const QString o = QStringLiteral("eq%1-").arg(b);
        set.params.append({o + QLatin1String("freq"), ParamType::Number,
            QStringLiteral("clip equalizer band %1 frequency in Hz, 20 - 20000").arg(b), false, {},
            {}});
        set.params.append({o + QLatin1String("gain"), ParamType::Number,
            QStringLiteral("clip equalizer band %1 gain in dB, -24 - 24").arg(b), false, {}, {}});
        set.params.append({o + QLatin1String("q"), ParamType::Number,
            QStringLiteral("clip equalizer band %1 Q, 0.1 - 18").arg(b), false, {}, {}});
    }
    registry.add({set, runSet});
}

} // namespace kk::commands
