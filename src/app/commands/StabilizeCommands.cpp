// SPDX-License-Identifier: GPL-3.0-or-later
// Command: clip.stabilize (spec section 58). Measures the camera movement of a clip, as the
// Inspector's Stabilize button does, and stores it with the settings in one undo step. The
// measurement runs in the calling thread and reports its progress through the command's progress
// callback; a window that runs the command live is busy meanwhile.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "compositing/tracking/StabilizeJob.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <algorithm>

namespace kk::commands {

namespace st = stabilize;
using timelineedit::runEdit;

namespace {

CommandResult checkRange(const char* option, double value, double min, double max)
{
    if (!(value >= min && value <= max))
        return fail("out_of_range",
            QStringLiteral("--%1 must be between %2 and %3")
                .arg(QString::fromLatin1(option))
                .arg(min)
                .arg(max));
    return CommandResult::success({}, {});
}

CommandResult runStabilize(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = project.timeline()->data();
    const quint64 id = quint64(args.integer(QStringLiteral("clip")));
    FoundClip found;
    if (auto bad = findClipOrFail(d, id, found))
        return *bad;
    if (d.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));

    // The settings: what the clip has, changed by the options given.
    st::Settings settings = found.clip->stabilization.settings;
    if (args.has(QStringLiteral("mode"))) {
        const QString text = args.string(QStringLiteral("mode"));
        settings.mode = st::modeFromId(text, settings.mode);
        if (st::modeId(settings.mode) != text)
            return fail("bad_value",
                QStringLiteral("--mode must be perspective, similarity or translation"));
    }
    struct Number
    {
        const char* option;
        double* target;
        double min;
        double max;
    };
    for (const Number& n : {Number {"smooth", &settings.smooth, st::kMinSmooth, st::kMaxSmooth},
             Number {"strength", &settings.strength, st::kMinStrength, st::kMaxStrength},
             Number {"cropping-ratio", &settings.croppingRatio, st::kMinCroppingRatio,
                 st::kMaxCroppingRatio}}) {
        const QString name = QString::fromLatin1(n.option);
        if (!args.has(name))
            continue;
        const double value = args.number(name);
        if (auto r = checkRange(n.option, value, n.min, n.max); !r.ok)
            return r;
        *n.target = value;
    }
    if (args.has(QStringLiteral("zoom")))
        settings.zoom = args.boolean(QStringLiteral("zoom"));
    if (args.has(QStringLiteral("camera-lock")))
        settings.cameraLock = args.boolean(QStringLiteral("camera-lock"));

    // Keep the old measurement when asked and still valid for the Mode.
    const auto& old = found.clip->stabilization.data;
    if (args.boolean(QStringLiteral("settings-only"))) {
        if (!old || old->mode() != settings.mode)
            return fail("not_analysed",
                QStringLiteral("the clip has no measurement for the mode %1; run clip.stabilize "
                               "without --settings-only")
                    .arg(st::modeId(settings.mode)));
        runEdit(&project, QStringLiteral("Stabilize Clip"), [&](TimelineData& data) {
            ClipData* clip = editops::clipById(data, id);
            tracking::applyStabilization(*clip, settings, old);
            return true;
        });
        return CommandResult::success({{QStringLiteral("analysed"), false}},
            QStringLiteral("stabilization settings of clip %1 changed").arg(id));
    }

    const MediaItem* item = project.mediaPool()->item(found.clip->mediaId);
    if (found.clip->kind != ClipKind::Media)
        return fail(
            "cannot_stabilize", QStringLiteral("only clips of video files can be stabilized"));
    if (!item)
        return fail("unknown_media", QStringLiteral("the media of the clip is missing"));
    QString why;
    const auto job = tracking::planStabilizeJob(*found.clip, *item, d.fps, settings.mode, &why);
    if (!job)
        return fail("cannot_stabilize", why);

    const tracking::StabilizeOutcome outcome
        = tracking::runStabilizeJob(*job, [&ctx](int done, int total) {
              if (ctx.progress)
                  ctx.progress(double(done) / std::max(1, total),
                      QStringLiteral("analysing frame %1 of %2").arg(done).arg(total));
          });
    if (!outcome.data)
        return fail("analysis_failed",
            outcome.error.isEmpty() ? QStringLiteral("cancelled") : outcome.error);

    runEdit(&project, QStringLiteral("Stabilize Clip"), [&](TimelineData& data) {
        ClipData* clip = editops::clipById(data, id);
        tracking::applyStabilization(*clip, settings, outcome.data);
        return true;
    });
    QJsonObject result {{QStringLiteral("clip"), qint64(id)},
        {QStringLiteral("frames"), qint64(outcome.data->frames())},
        {QStringLiteral("firstSourceFrame"), qint64(outcome.data->firstFrame())},
        {QStringLiteral("lostPairs"), outcome.lost}, {QStringLiteral("analysed"), true},
        {QStringLiteral("mode"), st::modeId(settings.mode)}};
    QString text = QStringLiteral("stabilized clip %1: %2 frames analysed")
                       .arg(id)
                       .arg(outcome.data->frames());
    if (outcome.lost > 0)
        text += QStringLiteral(", %1 of %2 frame pairs could not be matched")
                    .arg(outcome.lost)
                    .arg(outcome.pairs);
    return CommandResult::success(std::move(result), text);
}

} // namespace

void registerStabilizeCommands(CommandRegistry& registry)
{
    registry.add(
        {{QStringLiteral("clip.stabilize"),
             QStringLiteral(
                 "Stabilize a shaky video clip: measure its camera movement (this takes a while, "
                 "progress is reported) and switch Stabilization on. Works on any trim, speed or "
                 "direction of the clip; the result is stored with the clip. Options not given "
                 "keep the clip's current stabilization settings."),
             {{QStringLiteral("clip"), ParamType::Integer,
                  QStringLiteral("clip id (see clip.list)"), true, {}, {}},
                 {QStringLiteral("mode"), ParamType::String,
                     QStringLiteral("what camera movement is corrected: perspective (full), "
                                    "similarity (pan, tilt, zoom, rotation) or translation (pan "
                                    "and tilt only)"),
                     false, QStringLiteral("perspective"),
                     {QStringLiteral("perspective"), QStringLiteral("similarity"),
                         QStringLiteral("translation")}},
                 {QStringLiteral("smooth"), ParamType::Number,
                     QStringLiteral("how smooth the new camera path is, 0 - 1"), false,
                     QStringLiteral("0.25"), {}},
                 {QStringLiteral("strength"), ParamType::Number,
                     QStringLiteral("how much of the correction is applied, -1 - 1 (0 = off, "
                                    "negative = the opposite way)"),
                     false, QStringLiteral("1"), {}},
                 {QStringLiteral("cropping-ratio"), ParamType::Number,
                     QStringLiteral("how far zoom may crop, 0.25 - 1"), false, QStringLiteral("1"),
                     {}},
                 {QStringLiteral("zoom"), ParamType::Boolean,
                     QStringLiteral("true: zoom in to hide the empty edges"), false,
                     QStringLiteral("false"), {}},
                 {QStringLiteral("camera-lock"), ParamType::Boolean,
                     QStringLiteral("true: remove all camera movement, like a tripod (smooth and "
                                    "cropping-ratio then do nothing)"),
                     false, QStringLiteral("false"), {}},
                 {QStringLiteral("settings-only"), ParamType::Boolean,
                     QStringLiteral("true: do not measure again, only change the settings of the "
                                    "stored measurement (fails if there is none for the mode)"),
                     false, QStringLiteral("false"), {}}},
             ProjectUse::Modify, true},
            runStabilize});
}

} // namespace kk::commands
