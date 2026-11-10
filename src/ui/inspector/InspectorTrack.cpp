// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorTrack — the Track buttons of the tracking effects in the Effects tab (spec section 70):
// Track Backward, Track Forward, Clear Track, Cancel, a progress bar and a status line under the
// parameters of Match Move. Part of InspectorPanel. The tracking runs on the TrackerRunner
// (off the UI thread) over pictures the playback engine renders from a private copy of the
// timeline; the finished track is stored with the effect in one undo step.
#include "compositing/tracking/EffectTrackJob.h"
#include "core/EffectTrack.h"
#include "core/MatchMove.h"
#include "core/Project.h"
#include "core/SurfaceTracker.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamAccess.h"
#include "ui/inspector/ParamGroupBox.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

#include <algorithm>

namespace kk {

void InspectorPanel::startEffectTrack(quint64 clipId, quint64 effectId, bool forward)
{
    auto say = [&](const QString& text) {
        m_trackMessage = text;
        m_trackMessageEffect = effectId;
        refreshValues();
    };
    if (m_effectTracker->isRunning())
        return;
    if (!m_ctx.playback) {
        say(tr("Cannot track: there is no playback engine."));
        return;
    }
    QString why;
    auto plan = tracking::planEffectTrack(m_ctx.project->timeline()->data(), clipId, effectId,
        m_ctx.project->timeline()->playhead(), forward, &why);
    if (!plan) {
        say(tr("Cannot track: %1.").arg(why));
        return;
    }
    m_trackPlan = std::make_shared<tracking::EffectTrackPlan>(std::move(*plan));
    tracking::TrackJob job = m_trackPlan->job;
    PlaybackEngine* engine = m_ctx.playback;
    const auto timeline = m_trackPlan->timeline;
    const QSize size = m_trackPlan->renderSize;
    job.source = [engine, timeline, size](int64_t frame) {
        return engine->renderImageOf(timeline, frame, size);
    };
    if (!m_effectTracker->start(std::move(job))) {
        m_trackPlan.reset();
        say(tr("Cannot track: nothing to track."));
        return;
    }
    m_trackMessage.clear();
    m_trackMessageEffect = effectId;
    refreshValues();
}

void InspectorPanel::onEffectTrackFinished(tracking::TrackOutcome outcome, const QString& reason)
{
    if (!m_trackPlan)
        return;
    const auto plan = std::move(m_trackPlan);
    m_trackPlan.reset();
    const std::vector<tracking::TrackSample>& samples = m_effectTracker->samples();
    QString message;
    if (samples.size() < 2) {
        message = outcome == tracking::TrackOutcome::Stopped
            ? tr("Tracking cancelled. The effect is unchanged.")
            : tr("Tracking failed: %1").arg(reason.isEmpty() ? tr("nothing was found.") : reason);
    } else if (!clip(plan->clipId)) {
        message = tr("The clip was removed; the track was dropped.");
    } else {
        const auto table = tracking::tableFromSamples(*plan, samples);
        if (table) {
            const quint64 clipId = plan->clipId, effectId = plan->effectId;
            editClip(tr("Track"), clipId, [table, effectId](ClipData& c) {
                tracking::storeEffectTrack(c, effectId, *table);
            });
        }
        message = outcome == tracking::TrackOutcome::Finished
            ? tr("Tracked %1 frames.").arg(samples.size())
            : tr("Tracked %1 frames, then stopped: %2").arg(samples.size()).arg(reason);
    }
    m_trackMessage = message;
    m_trackMessageEffect = plan->effectId;
    refreshValues();
}

void InspectorPanel::buildTrackingControls(
    ParamGroupBox* group, const EffectInstance& fx, quint64 clipId)
{
    const bool surfaceFx = fx.type == QLatin1String(fxid::SurfaceTracker);
    if (fx.type != QLatin1String(fxid::MatchMove) && !surfaceFx)
        return;
    const quint64 effectId = fx.id;
    auto* buttons = new QWidget;
    auto* layout = new QHBoxLayout(buttons);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* backward = new QPushButton(tr("Track Backward"));
    backward->setObjectName(QStringLiteral("TrackBackward"));
    backward->setToolTip(tr("Track from the playhead back to the start of the clip. Put the "
                            "region over the feature at the playhead first."));
    auto* forward = new QPushButton(tr("Track Forward"));
    forward->setObjectName(QStringLiteral("TrackForward"));
    forward->setToolTip(tr("Track from the playhead on to the end of the clip. Put the region "
                           "over the feature at the playhead first."));
    auto* clear = new QPushButton(tr("Clear Track"));
    clear->setObjectName(QStringLiteral("TrackClear"));
    auto* cancel = new QPushButton(tr("Cancel"));
    cancel->setObjectName(QStringLiteral("TrackCancel"));
    layout->addWidget(backward);
    layout->addWidget(forward);
    layout->addWidget(clear);
    layout->addWidget(cancel);
    layout->addStretch(1);
    group->addWidgetRow(QString(), buttons);
    auto* progress = new QProgressBar;
    progress->setObjectName(QStringLiteral("TrackProgress"));
    group->addWidgetRow(QString(), progress);
    auto* status = new QLabel;
    status->setObjectName(QStringLiteral("TrackStatus"));
    status->setWordWrap(true);
    group->addWidgetRow(QString(), status);

    connect(backward, &QPushButton::clicked, this,
        [this, clipId, effectId] { startEffectTrack(clipId, effectId, false); });
    connect(forward, &QPushButton::clicked, this,
        [this, clipId, effectId] { startEffectTrack(clipId, effectId, true); });
    connect(cancel, &QPushButton::clicked, this, [this] { m_effectTracker->stop(); });
    connect(clear, &QPushButton::clicked, this, [this, clipId, effectId] {
        editClip(tr("Clear Track"), clipId,
            [effectId](ClipData& c) { tracking::clearEffectTrack(c, effectId); });
        m_trackMessage.clear();
    });

    group->addHeaderRefresh([this, surfaceFx, effectId, backward, forward, clear, cancel, progress,
                                status](const ClipData& cd) {
        const EffectInstance* e = paramaccess::effect(cd, effectId);
        if (!e)
            return;
        const bool running = m_effectTracker->isRunning();
        const bool mine = running && m_trackPlan && m_trackPlan->effectId == effectId;
        backward->setEnabled(!running);
        forward->setEnabled(!running);
        const auto table = efftrack::decode(e->params.staticValue(fxkeys::TrackData).toString());
        clear->setEnabled(!running && table);
        cancel->setVisible(mine);
        progress->setVisible(mine);
        if (mine) {
            progress->setMaximum(std::max<int>(1, int(m_effectTracker->totalFrames())));
            progress->setValue(int(m_effectTracker->samples().size()));
        }
        QString text;
        if (mine)
            text = tr("Tracking...");
        else if (m_trackMessageEffect == effectId && !m_trackMessage.isEmpty())
            text = m_trackMessage;
        else if (surfaceFx ? surface::needsTracking(*e) : matchmove::needsTracking(*e))
            text = tr("The region or the tracking changed since the track. Track again.");
        else if (!table)
            text = tr("Not tracked yet. Put the region over the feature, then press Track "
                      "Forward or Track Backward.");
        else
            text = tr("Tracked: %1 frames.").arg(table->keys());
        if (status->text() != text)
            status->setText(text);
    });
}

} // namespace kk
