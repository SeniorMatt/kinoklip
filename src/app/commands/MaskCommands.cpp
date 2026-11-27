// SPDX-License-Identifier: GPL-3.0-or-later
// Command: mask.follow (spec section 187). Makes a mask of a clip follow the subject under it, as
// the Inspector's Follow Subject > Track button does: tracks from a frame to the end of the clip
// and back to its start with the planar tracker, then keys the mask's position, scale and
// rotation in one undo step. The tracking runs in the calling thread (an event loop waits for
// it) and reports its progress through the command's progress callback.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "compositing/tracking/MaskTrackJob.h"
#include "compositing/tracking/TrackerRunner.h"
#include "core/EditOps.h"
#include "core/Mask.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "render/Compositor.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <QEventLoop>

namespace kk::commands {

using timelineedit::runEdit;

namespace {

/// Tracks @p plan's job with @p compositor to the end; the samples (empty on failure) and a
/// sentence in @p error.
std::vector<tracking::TrackSample> trackPlan(const tracking::MaskTrackPlan& plan,
    Compositor& compositor, const ProgressFn& progress, QString* error)
{
    tracking::TrackJob job = plan.track.job;
    const auto timeline = plan.track.timeline;
    const QSize size = plan.track.renderSize;
    Compositor* gpu = &compositor;
    job.source = [gpu, timeline, size](int64_t frame) {
        RenderRequest r;
        r.timeline = timeline;
        r.frame = frame;
        r.outSize = size;
        r.outputs = OutputImage;
        r.subtitles = false;
        return gpu->renderAsync(std::move(r));
    };
    tracking::TrackerRunner runner;
    QEventLoop loop;
    QObject::connect(&runner, &tracking::TrackerRunner::finished, &loop,
        [&](tracking::TrackOutcome outcome, const QString& reason) {
            if (outcome == tracking::TrackOutcome::Failed && error)
                *error = reason;
            loop.quit();
        });
    if (progress) {
        QObject::connect(
            &runner, &tracking::TrackerRunner::progress, &loop, [&](int done, int total) {
                progress(double(done) / std::max(1, total),
                    QStringLiteral("tracking frame %1 of %2").arg(done).arg(total));
            });
    }
    if (!runner.start(std::move(job)))
        return {};
    loop.exec();
    return runner.samples();
}

CommandResult runMaskFollow(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = project.timeline()->data();
    const quint64 clipId = quint64(args.integer(QStringLiteral("clip")));
    const quint64 maskId = quint64(args.integer(QStringLiteral("mask")));
    FoundClip found;
    if (auto bad = findClipOrFail(d, clipId, found))
        return *bad;
    if (found.track.type != TrackType::Video)
        return fail(
            "cannot_track", QStringLiteral("only masks of video clips can follow a subject"));
    if (d.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));
    bool hasMask = false;
    for (const MaskData& m : found.clip->masks)
        hasMask = hasMask || m.id == maskId;
    if (!hasMask)
        return fail("unknown_mask",
            QStringLiteral("clip %1 has no mask %2 (see clip.list)").arg(clipId).arg(maskId));

    std::optional<int64_t> at;
    QString timeError;
    if (args.has(QStringLiteral("at"))) {
        at = args.time(QStringLiteral("at"), d.fps, &timeError);
        if (!at)
            return fail("bad_value", timeError);
    }
    const int64_t playhead = at.value_or(found.clip->start);
    if (!found.clip->contains(playhead))
        return fail("out_of_range",
            QStringLiteral("frame %1 is outside clip %2 (%3 to %4)")
                .arg(playhead)
                .arg(clipId)
                .arg(found.clip->start)
                .arg(found.clip->end() - 1));

    Compositor compositor;
    compositor.setMediaPaths(makeMediaPathMap(project.mediaPool()->state()));
    std::vector<tracking::TrackSample> all;
    std::optional<tracking::MaskTrackPlan> first;
    QString problem;
    for (const bool forward : {true, false}) {
        QString why;
        auto plan = tracking::planMaskTrack(d, clipId, maskId, playhead, forward, &why);
        if (!plan) {
            if (!first)
                return fail("cannot_track", why);
            continue;
        }
        QString error;
        const auto samples = trackPlan(*plan, compositor, ctx.progress, &error);
        all.insert(all.end(), samples.begin(), samples.end());
        if (!error.isEmpty())
            problem = error;
        if (!first)
            first = std::move(plan);
    }
    if (!first || all.size() < 2)
        return fail("tracking_failed",
            problem.isEmpty() ? QStringLiteral("nothing could be tracked: the area under the mask "
                                               "needs texture that stays visible")
                              : problem);

    bool stored = false;
    runEdit(&project, QStringLiteral("Follow Subject"), [&](TimelineData& data) {
        ClipData* clip = editops::clipById(data, clipId);
        stored = clip
            && tracking::storeMaskTrack(
                *clip, *first, all, QSizeF(data.width, data.height), data.fps);
        return stored;
    });
    if (!stored)
        return fail("cannot_track", QStringLiteral("the mask could not be keyed"));
    return CommandResult::success(
        {{QStringLiteral("clip"), qint64(clipId)}, {QStringLiteral("mask"), qint64(maskId)},
            {QStringLiteral("frames"), qint64(all.size())}},
        QStringLiteral("mask %1 of clip %2 follows its subject (%3 frames tracked)")
            .arg(maskId)
            .arg(clipId)
            .arg(all.size()));
}

} // namespace

void registerMaskCommands(CommandRegistry& registry)
{
    registry.add(
        {{QStringLiteral("mask.follow"),
             QStringLiteral(
                 "Make a mask of a video clip follow the subject under it: the area under the "
                 "mask is tracked from a frame to the end of the clip and back to its start (this "
                 "takes a while, progress is reported), and the mask's position, scale and "
                 "rotation get a key on every frame. The mask keeps its place on the starting "
                 "frame. The clip is tracked without its masks."),
             {{QStringLiteral("clip"), ParamType::Integer,
                  QStringLiteral("clip id (see clip.list)"), true, {}, {}},
                 {QStringLiteral("mask"), ParamType::Integer,
                     QStringLiteral("mask id (see clip.list)"), true, {}, {}},
                 {QStringLiteral("at"), ParamType::Time,
                     QStringLiteral("timeline frame the mask is placed on, inside the clip "
                                    "(default: the first frame of the clip)"),
                     false, {}, {}}},
             ProjectUse::Modify, true},
            runMaskFollow});
}

} // namespace kk::commands
