// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel, multicam part: Create New Multicam Clip Using Selected Clips and Sync and
// Place on Timeline (waveform / timecode / in points).
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/mediapool/MulticamDialog.h"
#include "ui/mediapool/MulticamSync.h"

#include <QAction>
#include <QMenu>
#include <QMessageBox>

namespace kk {

namespace {

/// Action ids of the media pool's multicam commands (ActionRegistry, keymap).
const char* const kNewMulticam = "mediapool.new_multicam";
const char* const kSyncWaveform = "mediapool.sync_waveform";
const char* const kSyncTimecode = "mediapool.sync_timecode";
const char* const kSyncInPoints = "mediapool.sync_in_points";

} // namespace

QStringList MediaPoolPanel::syncableIds(const QStringList& ids) const
{
    QStringList out;
    for (const QString& id : ids) {
        const MediaItem* item = m_ctx.project->mediaPool()->item(id);
        if (item && !item->isTimeline() && !item->info.isStillImage && item->info.durationUs > 0
            && (item->info.hasVideo || item->info.hasAudio))
            out.append(id);
    }
    return out;
}

/// The SyncInput of every id (all syncable).
static QVector<SyncInput> syncInputs(
    const Project& project, const QStringList& ids, const MediaPoolPanel::InPointProvider& inPoints)
{
    QVector<SyncInput> inputs;
    for (const QString& id : ids) {
        const MediaItem* item = project.mediaPool()->item(id);
        inputs.append({id, item->name, item->path, item->info, inPoints ? inPoints(id) : 0});
    }
    return inputs;
}

quint64 MediaPoolPanel::createMulticamClip(const QStringList& ids, const QString& name,
    multicam::SyncMethod sync, bool namesFromClips, bool moveSources)
{
    const QStringList usable = syncableIds(ids);
    if (usable.isEmpty())
        return 0;
    const QVector<SyncInput> inputs = syncInputs(*m_ctx.project, usable, m_inPoints);
    const std::optional<QVector<int64_t>> offsets
        = runSyncWithFeedback(this, inputs, sync, tr("New Multicam Clip"));
    if (!offsets)
        return 0;
    QVector<multicam::AngleSource> sources;
    Rational fps {0, 1};
    int width = 0, height = 0;
    for (qsizetype k = 0; k < inputs.size(); ++k) {
        const SyncInput& in = inputs[k];
        const QString angle
            = namesFromClips ? multicam::defaultAngleName(in.name) : tr("Angle %1").arg(k + 1);
        sources.append({in.mediaId, angle, in.info, (*offsets)[k]});
        // The first clip with video sets the multicam clip's rate and picture size.
        if (!fps.isValid() && in.info.hasVideo && in.info.frameRate.isValid()) {
            fps = in.info.frameRate;
            width = in.info.displayWidth > 0 ? in.info.displayWidth : in.info.width;
            height = in.info.displayHeight > 0 ? in.info.displayHeight : in.info.height;
        }
    }
    const ProjectSettings settings = m_ctx.project->settings();
    if (!fps.isValid() || width <= 0 || height <= 0) {
        fps = settings.fps;
        width = settings.width;
        height = settings.height;
    }
    QString error;
    const quint64 uid = m_ctx.project->createMulticamClip(
        sources, name, fps, width, height, currentBin(), moveSources, &error);
    if (!uid)
        QMessageBox::warning(this, tr("New Multicam Clip"), error);
    return uid;
}

void MediaPoolPanel::newMulticamClipWithDialog()
{
    const QStringList ids = syncableIds(selectedMediaIds());
    if (ids.isEmpty()) {
        QMessageBox::information(this, tr("New Multicam Clip"),
            tr("Select the video or audio clips of the angles first."));
        return;
    }
    const std::optional<MulticamOptions> options = askMulticamOptions(
        this, int(ids.size()), m_ctx.project->uniqueTimelineName(tr("Multicam Clip")));
    if (!options)
        return;
    createMulticamClip(
        ids, options->name, options->sync, options->namesFromClips, options->moveSources);
}

int MediaPoolPanel::syncAndPlaceOnTimeline(const QStringList& ids, multicam::SyncMethod sync)
{
    const QStringList usable = syncableIds(ids);
    if (usable.size() < 2) {
        QMessageBox::information(this, tr("Sync and Place on Timeline"),
            tr("Select two or more video or audio clips to sync."));
        return 0;
    }
    const QVector<SyncInput> inputs = syncInputs(*m_ctx.project, usable, m_inPoints);
    const std::optional<QVector<int64_t>> offsets
        = runSyncWithFeedback(this, inputs, sync, tr("Sync and Place on Timeline"));
    if (!offsets)
        return 0;
    QVector<multicam::AngleSource> sources;
    for (qsizetype k = 0; k < inputs.size(); ++k)
        sources.append({inputs[k].mediaId, multicam::defaultAngleName(inputs[k].name),
            inputs[k].info, (*offsets)[k]});
    Project* project = m_ctx.project;
    project->beginEdit(tr("Sync and Place on Timeline"));
    QString error;
    const auto placed = multicam::placeSynced(
        project->timeline()->mutableData(), sources, project->timeline()->playhead(), &error);
    if (!placed) {
        project->cancelEdit();
        QMessageBox::warning(this, tr("Sync and Place on Timeline"), error);
        return 0;
    }
    project->endEdit();
    return int(placed->size());
}

void MediaPoolPanel::addMulticamMenu(QMenu& menu, const QStringList& ids)
{
    const QStringList usable = syncableIds(ids);
    if (usable.isEmpty())
        return;
    QAction* create = menu.addAction(icon(QStringLiteral("multicam_clip")),
        tr("Create New Multicam Clip Using Selected Clips…"), this,
        &MediaPoolPanel::newMulticamClipWithDialog);
    create->setObjectName(QLatin1String(kNewMulticam));
    if (usable.size() < 2)
        return;
    QMenu* sync = menu.addMenu(tr("Sync and Place on Timeline"));
    sync->setObjectName(QStringLiteral("SyncAndPlaceMenu"));
    const std::pair<multicam::SyncMethod, QString> methods[] = {
        {multicam::SyncMethod::Waveform, tr("Based on Waveform")},
        {multicam::SyncMethod::Timecode, tr("Based on Timecode")},
        {multicam::SyncMethod::InPoints, tr("Based on In Points")},
    };
    for (const auto& [method, text] : methods) {
        const multicam::SyncMethod m = method;
        sync->addAction(text, this, [this, usable, m] { syncAndPlaceOnTimeline(usable, m); });
    }
}

void MediaPoolPanel::connectMulticamActions()
{
    if (!m_ctx.actions)
        return;
    // Menu bar / keyboard: only the visible media pool acts (Media and Edit page both have one).
    auto on = [this](const char* id, const std::function<void()>& run) {
        if (!m_ctx.actions->contains(QLatin1String(id)))
            return;
        connect(m_ctx.actions->action(QLatin1String(id)), &QAction::triggered, this, [this, run] {
            if (isVisible())
                run();
        });
    };
    on(kNewMulticam, [this] { newMulticamClipWithDialog(); });
    on(kSyncWaveform,
        [this] { syncAndPlaceOnTimeline(selectedMediaIds(), multicam::SyncMethod::Waveform); });
    on(kSyncTimecode,
        [this] { syncAndPlaceOnTimeline(selectedMediaIds(), multicam::SyncMethod::Timecode); });
    on(kSyncInPoints,
        [this] { syncAndPlaceOnTimeline(selectedMediaIds(), multicam::SyncMethod::InPoints); });
}

} // namespace kk
