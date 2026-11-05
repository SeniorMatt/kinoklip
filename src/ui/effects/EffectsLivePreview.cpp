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
    std::optional<PreparedScene> prepared
        = PreparedScene::prepare(snapshot, playhead, selected, descriptorId);
    if (!prepared)
        return std::nullopt;
    return prepared->sceneAt(prepared->stepAt(position));
}

std::optional<PreparedScene> PreparedScene::prepare(const TimelineData& snapshot, int64_t playhead,
    const QVector<quint64>& selected, const QString& descriptorId)
{
    const EffectDescriptor* desc = EffectRegistry::instance().find(descriptorId);
    if (!desc)
        return std::nullopt;
    PreparedScene scene;
    switch (desc->category) {
    case EffectCategory::VideoEffect: {
        const quint64 clipId = targetClip(snapshot, playhead, selected);
        if (!clipId)
            return std::nullopt;
        scene.m_timeline = std::make_shared<TimelineData>(snapshot);
        ClipData* clip = editops::clipById(*scene.m_timeline, clipId);
        clip->effects.append(
            EffectRegistry::instance().createEffect(descriptorId, scene.m_timeline->allocateId()));
        scene.m_kind = Kind::Fixed;
        scene.m_frame = playhead;
        return scene;
    }
    case EffectCategory::Transition: {
        const quint64 clipId = targetClip(snapshot, playhead, selected);
        const ClipData* clip = editops::clipById(snapshot, clipId);
        if (!clip)
            return std::nullopt;
        // The end of the clip nearer to the playhead, as a double-click would choose; a tail
        // that touches the next clip is that clip's head (the cut keeps one transition).
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

        scene.m_timeline = std::make_shared<TimelineData>(snapshot);
        if (!editops::placeTransition(*scene.m_timeline, storedOn, head, transition))
            return std::nullopt;
        const auto window = editops::transitionWindow(*scene.m_timeline, storedOn, head);
        if (!window || window->second <= window->first)
            return std::nullopt;
        scene.m_kind = Kind::Transition;
        scene.m_frame = window->first;
        scene.m_length = window->second - window->first;
        return scene;
    }
    case EffectCategory::Title:
    case EffectCategory::Generator: {
        std::optional<ClipData> clip = effectactions::makeGeneratorClip(descriptorId, snapshot.fps);
        if (!clip || clip->duration < 1)
            return std::nullopt;
        // Step 0: the playhead at the clip's start; sceneAt() moves the clip so that the
        // playhead sits `step` frames into it (titles animate), never before the timeline start.
        clip->start = std::max<int64_t>(0, playhead);
        const int64_t length = clip->duration;
        scene.m_timeline = std::make_shared<TimelineData>(snapshot);
        const int track = editops::addTrack(*scene.m_timeline, TrackType::Video);
        if (!editops::overwriteClip(
                *scene.m_timeline, TrackRef {TrackType::Video, track}, std::move(*clip)))
            return std::nullopt;
        scene.m_kind = Kind::Clip;
        scene.m_frame = playhead;
        scene.m_length = length;
        scene.m_track = size_t(track);
        return scene;
    }
    case EffectCategory::Effects: // the Adjustment Clip: nothing to see with its defaults
    case EffectCategory::AudioEffect:
    case EffectCategory::AudioTransition:
    case EffectCategory::ClipProperties:
        break;
    }
    return std::nullopt;
}

int64_t PreparedScene::stepAt(double position, int64_t current) const
{
    if (m_kind == Kind::Fixed || m_length <= 1)
        return 0;
    position = std::clamp(position, 0.0, 1.0);
    const double exact = position * double(m_length - 1);
    if (current >= 0 && current < m_length
        && std::abs(exact - double(current)) < 0.5 + kJitterFrames)
        return current; // jitter around a step boundary
    return offsetInto(m_length, position);
}

Scene PreparedScene::sceneAt(int64_t step)
{
    step = std::clamp<int64_t>(step, 0, m_length - 1);
    switch (m_kind) {
    case Kind::Fixed:
        break;
    case Kind::Transition:
        return Scene {m_timeline, m_frame + step};
    case Kind::Clip: {
        const int64_t start = std::max<int64_t>(0, m_frame - step);
        if (m_timeline->videoTracks[m_track].clips.front().start == start)
            break;
        if (m_timeline.use_count() > 1) {
            // A render still holds the scene: never change it under the render thread.
            m_timeline = std::make_shared<TimelineData>(*m_timeline);
            ++m_copies;
        } else {
            // Ours alone: the render thread's release of its reference happened before.
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        m_timeline->videoTracks[m_track].clips.front().start = start;
        break;
    }
    }
    return Scene {m_timeline, m_frame};
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
    dropPrepared();
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
    request(); // ensurePrepared() sees the new playhead / selection
}

void EffectsLivePreview::dropPrepared()
{
    m_preparedKey.reset();
    m_prepared.reset();
    m_shownStep = -1;
}

bool EffectsLivePreview::ensurePrepared()
{
    Timeline* timeline = m_ctx.project->timeline();
    PreparedKey key {m_wantedId, timeline->snapshot(), timeline->playhead(),
        m_ctx.selection ? m_ctx.selection->clips() : QVector<quint64> {}};
    if (m_preparedKey && m_preparedKey->id == key.id && m_preparedKey->snapshot == key.snapshot
        && m_preparedKey->playhead == key.playhead && m_preparedKey->selected == key.selected)
        return m_prepared.has_value();
    m_prepared
        = livepreview::PreparedScene::prepare(*key.snapshot, key.playhead, key.selected, key.id);
    m_preparedKey = std::move(key);
    ++m_generation;
    ++m_sceneBuilds;
    return m_prepared.has_value();
}

void EffectsLivePreview::request()
{
    if (m_wantedId.isEmpty() || !m_viewer || !m_ctx.project || !m_ctx.playback)
        return;
    if (m_running) {
        m_pending = true; // one render in flight; only the newest wish follows (latest wins)
        return;
    }
    m_pending = false;
    if (!ensurePrepared()) {
        // Nothing to show for this entry: the normal picture.
        m_shownId.clear();
        m_shownStep = -1;
        m_viewer->clearTemporaryFrame();
        return;
    }
    const bool showsThis = m_shownId == m_wantedId && m_shownGeneration == m_generation;
    const int64_t step
        = m_prepared->stepAt(m_wantedPosition, showsThis ? m_shownStep : int64_t(-1));
    if (showsThis && step == m_shownStep)
        return; // the same picture: a move within one frame, or an effect
    const livepreview::Scene scene = m_prepared->sceneAt(step);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_running = Running {m_ctx.playback->renderAlternative(scene.timeline, scene.frame, cancel),
        cancel, m_wantedId, step, m_generation};
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
    const Running done = std::move(*m_running);
    m_running.reset();
    if (done.id == m_wantedId && frame && frame->ok && m_viewer) {
        m_viewer->showTemporaryFrame(frame);
        m_shownId = done.id;
        m_shownStep = done.step;
        m_shownGeneration = done.generation;
        emit previewShown(done.id);
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
