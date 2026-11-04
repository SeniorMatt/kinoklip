// SPDX-License-Identifier: GPL-3.0-or-later
// EffectsLivePreview implementation: the preview scenes (pure functions on a timeline copy)
// and the debounced, cancellable render requests.
#include "ui/effects/EffectsLivePreview.h"

#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"
#include "ui/effects/EffectActions.h"
#include "ui/viewer/ViewerPanel.h"

#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace kk {

namespace livepreview {

namespace {

/// Frame @p position (0..1) of the way through @p length frames.
int64_t offsetInto(int64_t length, double position)
{
    if (length <= 1)
        return 0;
    return std::clamp<int64_t>(std::llround(position * double(length - 1)), 0, length - 1);
}

std::optional<Scene> effectScene(const TimelineData& snapshot, int64_t playhead,
    const QVector<quint64>& selected, const QString& descriptorId)
{
    const quint64 clipId = targetClip(snapshot, playhead, selected);
    if (!clipId)
        return std::nullopt;
    auto copy = std::make_shared<TimelineData>(snapshot);
    ClipData* clip = editops::clipById(*copy, clipId);
    clip->effects.append(EffectRegistry::instance().createEffect(descriptorId, copy->allocateId()));
    return Scene {copy, playhead};
}

std::optional<Scene> transitionScene(const TimelineData& snapshot, int64_t playhead,
    const QVector<quint64>& selected, const QString& descriptorId, double position)
{
    const quint64 clipId = targetClip(snapshot, playhead, selected);
    const ClipData* clip = editops::clipById(snapshot, clipId);
    if (!clip)
        return std::nullopt;
    // The end of the clip nearer to the playhead, as a double-click would choose; a tail that
    // touches the next clip is that clip's head (the cut keeps one transition).
    quint64 storedOn = clipId;
    bool head = std::abs(playhead - clip->start) <= std::abs(clip->end() - playhead);
    if (!head) {
        if (const quint64 next = editops::cutNeighbour(snapshot, clipId, false)) {
            storedOn = next;
            head = true;
        }
    }
    const ClipData* stored = editops::clipById(snapshot, storedOn);
    const std::optional<TransitionData>& existing
        = head ? stored->transitionIn : stored->transitionOut;
    TransitionData transition = EffectRegistry::instance().createTransition(descriptorId,
        existing ? existing->duration : effectactions::defaultTransitionFrames(snapshot.fps));
    if (existing)
        transition.alignment = existing->alignment;

    auto copy = std::make_shared<TimelineData>(snapshot);
    if (!editops::placeTransition(*copy, storedOn, head, transition))
        return std::nullopt;
    const auto window = editops::transitionWindow(*copy, storedOn, head);
    if (!window || window->second <= window->first)
        return std::nullopt;
    const int64_t frame = window->first + offsetInto(window->second - window->first, position);
    return Scene {copy, frame};
}

std::optional<Scene> generatorScene(
    const TimelineData& snapshot, int64_t playhead, const QString& descriptorId, double position)
{
    std::optional<ClipData> clip = effectactions::makeGeneratorClip(descriptorId, snapshot.fps);
    if (!clip || clip->duration < 1)
        return std::nullopt;
    // The playhead sits at `position` of the clip, so moving along the entry plays the
    // title's animation over the current picture; never before the timeline start.
    clip->start = std::max<int64_t>(0, playhead - offsetInto(clip->duration, position));
    auto copy = std::make_shared<TimelineData>(snapshot);
    const int track = editops::addTrack(*copy, TrackType::Video);
    if (!editops::overwriteClip(*copy, TrackRef {TrackType::Video, track}, std::move(*clip)))
        return std::nullopt;
    return Scene {copy, playhead};
}

} // namespace

quint64 targetClip(const TimelineData& data, int64_t playhead, const QVector<quint64>& selected)
{
    quint64 topmost = 0;
    for (auto track = data.videoTracks.crbegin(); track != data.videoTracks.crend(); ++track) {
        if (!track->enabled)
            continue;
        for (const ClipData& clip : track->clips) {
            if (!clip.enabled || !clip.contains(playhead))
                continue;
            if (selected.contains(clip.id))
                return clip.id;
            if (!topmost)
                topmost = clip.id;
        }
    }
    return topmost;
}

std::optional<Scene> buildScene(const TimelineData& snapshot, int64_t playhead,
    const QVector<quint64>& selected, const QString& descriptorId, double position)
{
    const EffectDescriptor* desc = EffectRegistry::instance().find(descriptorId);
    if (!desc)
        return std::nullopt;
    position = std::clamp(position, 0.0, 1.0);
    switch (desc->category) {
    case EffectCategory::VideoEffect:
        return effectScene(snapshot, playhead, selected, descriptorId);
    case EffectCategory::Transition:
        return transitionScene(snapshot, playhead, selected, descriptorId, position);
    case EffectCategory::Title:
    case EffectCategory::Generator:
        return generatorScene(snapshot, playhead, descriptorId, position);
    case EffectCategory::Effects: // the Adjustment Clip: nothing to see with its defaults
    case EffectCategory::AudioEffect:
    case EffectCategory::AudioTransition:
    case EffectCategory::ClipProperties:
        break;
    }
    return std::nullopt;
}

} // namespace livepreview

EffectsLivePreview::EffectsLivePreview(AppContext& ctx, ViewerPanel* viewer, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_viewer(viewer)
{
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &EffectsLivePreview::request);
    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, &EffectsLivePreview::collect);

    if (Project* project = m_ctx.project) {
        Timeline* timeline = project->timeline();
        connect(timeline, &Timeline::changed, this, &EffectsLivePreview::stop);
        connect(timeline, &Timeline::playheadChanged, this, &EffectsLivePreview::refresh);
        connect(project, &Project::activeTimelineAboutToChange, this, &EffectsLivePreview::stop);
        connect(project, &Project::projectReset, this, &EffectsLivePreview::stop);
    }
    if (m_ctx.selection)
        connect(m_ctx.selection, &SelectionModel::changed, this, &EffectsLivePreview::refresh);
    if (m_ctx.playback) {
        connect(m_ctx.playback, &PlaybackEngine::rateChanged, this, [this](double rate) {
            if (rate != 0.0)
                stop();
        });
    }
}

EffectsLivePreview::~EffectsLivePreview()
{
    cancelRunning();
}

void EffectsLivePreview::hover(const QString& descriptorId, double position)
{
    if (descriptorId.isEmpty()) {
        stop();
        return;
    }
    if (m_ctx.playback && m_ctx.playback->isPlaying())
        return; // Resolve previews on a still picture only
    m_wantedPosition = position;
    if (descriptorId != m_wantedId) {
        // Another entry: wait until the pointer rests; the old picture stays meanwhile.
        m_wantedId = descriptorId;
        cancelRunning();
        m_pending = false;
        m_debounce->start();
        return;
    }
    if (!m_debounce->isActive())
        request(); // scrubbing along the same entry
}

void EffectsLivePreview::stop()
{
    m_wantedId.clear();
    m_debounce->stop();
    m_pending = false;
    cancelRunning();
    m_shownId.clear();
    if (m_viewer)
        m_viewer->clearTemporaryFrame();
}

void EffectsLivePreview::flushDebounce()
{
    if (m_debounce->isActive()) {
        m_debounce->stop();
        request();
    }
}

void EffectsLivePreview::refresh()
{
    if (m_wantedId.isEmpty() || m_debounce->isActive())
        return;
    request();
}

void EffectsLivePreview::request()
{
    if (m_wantedId.isEmpty() || !m_viewer || !m_ctx.project || !m_ctx.playback)
        return;
    if (m_running) {
        m_pending = true; // one render in flight; the newest wish follows
        return;
    }
    m_pending = false;
    Timeline* timeline = m_ctx.project->timeline();
    const QVector<quint64> selected
        = m_ctx.selection ? m_ctx.selection->clips() : QVector<quint64> {};
    const std::optional<livepreview::Scene> scene = livepreview::buildScene(
        *timeline->snapshot(), timeline->playhead(), selected, m_wantedId, m_wantedPosition);
    if (!scene) {
        // Nothing to show for this entry: the normal picture.
        m_shownId.clear();
        m_viewer->clearTemporaryFrame();
        return;
    }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_running = Running {m_ctx.playback->renderAlternative(scene->timeline, scene->frame, cancel),
        cancel, m_wantedId};
    ++m_requests;
    if (!m_poll->isActive())
        m_poll->start();
}

void EffectsLivePreview::collect()
{
    if (!m_running) {
        m_poll->stop();
        return;
    }
    if (m_running->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    const RenderedFramePtr frame = m_running->future.get();
    const QString id = m_running->id;
    m_running.reset();
    if (id == m_wantedId && frame && frame->ok && m_viewer) {
        m_viewer->showTemporaryFrame(frame);
        m_shownId = id;
        emit previewShown(id);
    }
    if (m_pending)
        request();
    if (!m_running)
        m_poll->stop();
}

void EffectsLivePreview::cancelRunning()
{
    if (!m_running)
        return;
    // Skipped if the render thread has not picked it up yet; a std::future of a promise does
    // not block when it is dropped, so a render already running just finishes unseen.
    m_running->cancel->store(true);
    m_running.reset();
    ++m_cancelled;
}

} // namespace kk
