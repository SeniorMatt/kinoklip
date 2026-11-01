// SPDX-License-Identifier: GPL-3.0-or-later
// PlaybackEngine — transport state machine, clocks, look-ahead and presentation.
#include "playback/PlaybackEngine.h"

#include "core/MediaPool.h"
#include "core/Timeline.h"
#include "playback/AudioFeeder.h"
#include "playback/AudioOutput.h"
#include "playback/AudioRing.h"
#include "playback/PlaybackLog.h"
#include "render/Compositor.h"

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

/// Ring between mixer thread and device: 8192 frames = 171 ms at 48 kHz.
constexpr size_t kRingFrames = 8192;
/// Pre-roll before the device starts (frames in the ring), and how long to wait for it.
constexpr size_t kPrerollFrames = 2 * 1024;
constexpr int kPrerollTimeoutMs = 60;
/// Auto quality is re-evaluated this often while playing...
constexpr int64_t kQualityEvalUs = 500'000;
/// ...but not within this time after a change (the render-time average must refill).
constexpr int64_t kQualitySettleUs = 1'000'000;
/// Dropped frames per evaluation window that force a lower quality.
constexpr int64_t kDropsForDownshift = 3;

int64_t nowMs()
{
    return monotonicUs() / 1000;
}

Quality lower(Quality q)
{
    return q == Quality::Full ? Quality::Half : Quality::Quarter;
}

Quality higher(Quality q)
{
    return q == Quality::Quarter ? Quality::Half : Quality::Full;
}

} // namespace

PlaybackEngine::PlaybackEngine(QObject* parent)
    : PlaybackEngine(Options {}, parent)
{ }

PlaybackEngine::PlaybackEngine(Options options, QObject* parent)
    : QObject(parent)
    , m_scheduler(
          [this](RenderRequest request) { return m_compositor->renderAsync(std::move(request)); })
{
    init(std::move(options));
}

void PlaybackEngine::init(Options options)
{
    m_compositor
        = options.compositor ? std::move(options.compositor) : std::make_shared<Compositor>();
    if (!options.noAudio) {
        m_audio = options.audio ? std::move(options.audio) : createDefaultAudioOutput();
        m_ring = std::make_unique<AudioRing>(kRingFrames);
        m_feeder = std::make_unique<AudioFeeder>(m_ring.get());
    }
    m_tick.setTimerType(Qt::PreciseTimer);
    m_tick.setInterval(kTickMs);
    connect(&m_tick, &QTimer::timeout, this, &PlaybackEngine::onTick);
    m_shuttleTimer.setSingleShot(true);
    m_shuttleTimer.setTimerType(Qt::PreciseTimer);
    connect(
        &m_shuttleTimer, &QTimer::timeout, this, [this] { applyShuttle(m_shuttle.poll(nowMs())); });
}

PlaybackEngine::~PlaybackEngine()
{
    m_tick.stop();
    stopAudio();
    m_feeder.reset(); // joins the mixer thread before the ring goes away
    m_scheduler.clear();
    m_current.reset(); // before the compositor (texture pool) may be destroyed
}

// ---- Content ----

void PlaybackEngine::setTimeline(Timeline* timeline)
{
    if (timeline == m_timeline) {
        return;
    }
    setRateInternal(0.0);
    disconnect(m_timelineChanged);
    disconnect(m_playheadChanged);
    m_timeline = timeline;
    m_snapshot.reset();
    m_current.reset();
    if (m_timeline) {
        m_timelineChanged
            = connect(m_timeline, &Timeline::changed, this, &PlaybackEngine::onTimelineChanged);
        m_playheadChanged = connect(
            m_timeline, &Timeline::playheadChanged, this, &PlaybackEngine::onPlayheadChanged);
        m_position = m_timeline->playhead();
    } else {
        m_position = 0;
    }
    m_scheduler.clear();
    onTimelineChanged();
    Q_EMIT positionChanged(m_position);
}

void PlaybackEngine::setMediaPool(MediaPool* pool)
{
    disconnect(m_poolChanged);
    m_pool = pool;
    if (!m_pool) {
        setMediaPaths({});
        return;
    }
    m_poolChanged = connect(m_pool, &MediaPool::changed, this,
        [this] { setMediaPaths(makeMediaPathMap(m_pool->state())); });
    setMediaPaths(makeMediaPathMap(m_pool->state()));
}

void PlaybackEngine::setMediaPaths(std::shared_ptr<const MediaPathMap> media)
{
    m_media = std::move(media);
    if (m_feeder) {
        m_feeder->setMediaPaths(m_media);
    }
    updateParams(true);
}

void PlaybackEngine::onTimelineChanged()
{
    m_snapshot = m_timeline ? m_timeline->snapshot() : nullptr;
    m_clock.setFps(fps());
    if (m_feeder && m_snapshot) {
        m_feeder->setTimeline(m_snapshot);
    }
    if (!m_snapshot) {
        setRateInternal(0.0);
        m_scheduler.clear();
        m_current.reset();
        Q_EMIT frameReady(m_current);
        return;
    }
    updateParams(true);
}

void PlaybackEngine::onPlayheadChanged(int64_t frame)
{
    if (m_settingPlayhead) {
        return; // our own write-back
    }
    // Someone else moved the playhead (timeline scrub, Home/End, edit navigation).
    if (isPlaying()) {
        startMotion(m_rate, double(frame));
        return;
    }
    m_position = frame;
    Q_EMIT positionChanged(m_position);
    requestStill(false);
}

// ---- State ----

PlaybackEngine::State PlaybackEngine::state() const
{
    if (m_rate == 0.0) {
        return State::Stopped;
    }
    return m_rate == 1.0 ? State::Playing : State::Shuttling;
}

int64_t PlaybackEngine::duration() const
{
    return m_snapshot ? m_snapshot->duration() : 0;
}

Rational PlaybackEngine::fps() const
{
    return m_snapshot && m_snapshot->fps.isValid() ? m_snapshot->fps : Rational {};
}

bool PlaybackEngine::hasAudio() const
{
    return m_audio && m_audio->isAvailable();
}

void PlaybackEngine::range(int64_t* start, int64_t* end) const
{
    *start = 0;
    *end = duration();
    if ((m_loop || m_playInOut) && m_snapshot) {
        if (m_snapshot->markIn >= 0) {
            *start = m_snapshot->markIn;
        }
        if (m_snapshot->markOut > *start) {
            *end = m_snapshot->markOut;
        }
    }
    if (*end <= *start) {
        *end = *start + 1;
    }
}

bool PlaybackEngine::loopActive() const
{
    return m_loop;
}

int64_t PlaybackEngine::clampFrame(int64_t frame) const
{
    return std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, duration() - 1));
}

double PlaybackEngine::wrap(double unwrapped) const
{
    if (!m_rangeLoop) {
        return unwrapped;
    }
    const double len = double(m_rangeEnd - m_rangeStart);
    double offset = std::fmod(unwrapped - double(m_rangeStart), len);
    if (offset < 0) {
        offset += len;
    }
    return double(m_rangeStart) + offset;
}

// ---- Output ----

void PlaybackEngine::setOutputClient(const void* client, QSize pixelSize, bool canDrawTextures)
{
    const QSize before = outputSize();
    const bool texturesBefore = m_clients.isEmpty()
        ? false
        : std::all_of(
              m_clients.cbegin(), m_clients.cend(), [](const Client& c) { return c.textures; });
    m_clients.insert(client, Client {pixelSize, canDrawTextures});
    const bool texturesAfter = std::all_of(
        m_clients.cbegin(), m_clients.cend(), [](const Client& c) { return c.textures; });
    if (outputSize() != before || texturesBefore != texturesAfter) {
        updateParams(true);
    }
}

void PlaybackEngine::removeOutputClient(const void* client)
{
    if (m_clients.remove(client) > 0) {
        updateParams(false);
    }
}

QSize PlaybackEngine::outputSize() const
{
    const QSize native
        = m_snapshot ? QSize(m_snapshot->width, m_snapshot->height) : QSize(1920, 1080);
    if (m_clients.isEmpty() || native.isEmpty()) {
        return native;
    }
    QSize box;
    for (const Client& c : m_clients) {
        box = box.expandedTo(c.size);
    }
    if (box.isEmpty()) {
        return native;
    }
    // Timeline aspect fitted into the largest viewer; never more pixels than the timeline.
    QSize fitted = native.scaled(box, Qt::KeepAspectRatio);
    if (fitted.width() > native.width()) {
        fitted = native;
    }
    return fitted.expandedTo(QSize(1, 1));
}

void PlaybackEngine::setQualityMode(QualityMode mode)
{
    if (mode == m_qualityMode) {
        return;
    }
    m_qualityMode = mode;
    m_autoQuality = Quality::Full;
    m_scheduler.resetAverage();
    updateParams(true);
}

Quality PlaybackEngine::currentQuality() const
{
    switch (m_qualityMode) {
    case QualityMode::Full:
        return Quality::Full;
    case QualityMode::Half:
        return Quality::Half;
    case QualityMode::Quarter:
        return Quality::Quarter;
    case QualityMode::Auto:
        break;
    }
    // Auto: a paused picture is always sharp; playback adapts.
    return isPlaying() ? m_autoQuality : Quality::Full;
}

void PlaybackEngine::setMuted(bool muted)
{
    if (muted == m_muted) {
        return;
    }
    m_muted = muted;
    if (isPlaying()) {
        startMotion(m_rate, double(m_position)); // switch clock source
    }
}

void PlaybackEngine::setVolume(float volume)
{
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    if (m_audio) {
        m_audio->setVolume(m_volume);
    }
}

void PlaybackEngine::updateParams(bool rerender)
{
    RenderParams params;
    params.timeline = m_snapshot;
    params.media = m_media;
    params.outSize = outputSize();
    params.quality = currentQuality();
    const bool allTextures = !m_clients.isEmpty()
        && std::all_of(
            m_clients.cbegin(), m_clients.cend(), [](const Client& c) { return c.textures; });
    params.outputs = m_compositor->isGpu() && allTextures ? OutputTexture : OutputImage;
    const RenderParams& old = m_scheduler.params();
    const bool changed = old.timeline != params.timeline || old.media != params.media
        || old.outSize != params.outSize || old.quality != params.quality
        || old.outputs != params.outputs;
    if (changed) {
        m_scheduler.setParams(params);
    }
    if (params.quality != m_lastEmittedQuality) {
        m_lastEmittedQuality = params.quality;
        Q_EMIT qualityChanged(params.quality);
    }
    if (rerender && changed && !isPlaying() && m_snapshot) {
        requestStill(true);
    }
}

// ---- Statistics ----

PlaybackEngine::Stats PlaybackEngine::stats() const
{
    Stats s = m_stats;
    if (isPlaying() && m_motionStartUs > 0) {
        s.playSeconds += double(monotonicUs() - m_motionStartUs) * 1e-6;
    }
    s.averageRenderMs = m_scheduler.averageRenderMs();
    s.audioMaster = m_clock.isAudioMaster() && isPlaying();
    s.avOffsetFrames = m_clock.lastAudioError();
    return s;
}

void PlaybackEngine::resetStats()
{
    m_stats = Stats {};
    m_motionStartUs = isPlaying() ? monotonicUs() : 0;
}

// ---- Transport ----

void PlaybackEngine::play()
{
    setRate(1.0);
}

void PlaybackEngine::pause()
{
    setRate(0.0);
}

void PlaybackEngine::togglePlay()
{
    if (isPlaying()) {
        pause();
    } else {
        play();
    }
}

void PlaybackEngine::stop()
{
    m_playInOut = false;
    (void)m_shuttle.trigger(ShuttleControl::Key::K, m_rate, nowMs()); // reset hold state
    setRateInternal(0.0);
}

void PlaybackEngine::setRate(double rate)
{
    setRateInternal(std::clamp(rate, -ShuttleControl::kMaxRate, ShuttleControl::kMaxRate));
}

void PlaybackEngine::shuttleForward()
{
    applyShuttle(m_shuttle.trigger(ShuttleControl::Key::L, m_rate, nowMs()));
}

void PlaybackEngine::shuttleReverse()
{
    applyShuttle(m_shuttle.trigger(ShuttleControl::Key::J, m_rate, nowMs()));
}

void PlaybackEngine::shuttleStop()
{
    applyShuttle(m_shuttle.trigger(ShuttleControl::Key::K, m_rate, nowMs()));
}

void PlaybackEngine::setShuttleKeyHeld(ShuttleControl::Key key, bool held)
{
    applyShuttle(m_shuttle.setKeyHeld(key, held, nowMs()));
}

void PlaybackEngine::applyShuttle(const ShuttleControl::Command& cmd)
{
    switch (cmd.kind) {
    case ShuttleControl::Command::Kind::None:
        break;
    case ShuttleControl::Command::Kind::SetRate:
        setRate(cmd.rate);
        break;
    case ShuttleControl::Command::Kind::Step:
        step(cmd.step);
        break;
    }
    const int64_t deadline = m_shuttle.holdDeadlineMs();
    if (deadline >= 0) {
        m_shuttleTimer.start(int(std::max<int64_t>(0, deadline - nowMs())));
    }
}

void PlaybackEngine::step(int64_t frames)
{
    if (isPlaying()) {
        setRateInternal(0.0);
    }
    seek(clampFrame(m_position + frames));
}

void PlaybackEngine::stepSeconds(int direction)
{
    step(int64_t(std::llround(fps().toDouble())) * (direction >= 0 ? 1 : -1));
}

void PlaybackEngine::seek(int64_t frame)
{
    frame = std::max<int64_t>(0, frame);
    if (isPlaying()) {
        startMotion(m_rate, double(frame));
        return;
    }
    setPositionInternal(frame, true);
    requestStill(false);
}

void PlaybackEngine::setLoop(bool loop)
{
    if (loop == m_loop) {
        return;
    }
    m_loop = loop;
    Q_EMIT loopChanged(m_loop);
    if (isPlaying()) {
        startMotion(m_rate, double(m_position)); // re-capture the range (and re-anchor audio)
    }
}

void PlaybackEngine::playInToOut()
{
    if (!m_snapshot) {
        return;
    }
    if (isPlaying()) {
        setRateInternal(0.0);
    }
    m_playInOut = true;
    int64_t start = 0;
    int64_t end = 0;
    range(&start, &end);
    setPositionInternal(start, true);
    setRateInternal(1.0);
}

void PlaybackEngine::goToIn()
{
    if (m_snapshot && m_snapshot->markIn >= 0) {
        seek(m_snapshot->markIn);
    }
}

void PlaybackEngine::goToOut()
{
    if (m_snapshot && m_snapshot->markOut > 0) {
        seek(m_snapshot->markOut - 1);
    }
}

void PlaybackEngine::goToStart()
{
    seek(0);
}

void PlaybackEngine::goToEnd()
{
    seek(clampFrame(duration() - 1));
}

void PlaybackEngine::setRateInternal(double rate)
{
    if (!m_snapshot) {
        rate = 0.0;
    }
    if (rate == m_rate) {
        return;
    }
    const State before = state();
    const double from = double(m_position); // wrapped (loop) position
    m_rate = rate;
    if (rate == 0.0) {
        stopMotion();
    } else {
        startMotion(rate, from);
    }
    Q_EMIT rateChanged(m_rate);
    if (state() != before) {
        Q_EMIT stateChanged(state());
    }
}

void PlaybackEngine::startMotion(double rate, double fromFrame)
{
    const int64_t now = monotonicUs();
    if (m_motionStartUs > 0) {
        m_stats.playSeconds += double(now - m_motionStartUs) * 1e-6;
    }
    stopAudio();
    m_scheduler.clear(); // look-ahead for the old rate/position is useless now
    m_hasShownKey = false;

    m_rangeLoop = m_loop;
    range(&m_rangeStart, &m_rangeEnd);
    // The position may be a wrapped loop position: work on frames from here on.
    double start = std::floor(fromFrame);
    const bool outside = start < double(m_rangeStart) || start >= double(m_rangeEnd);
    if (m_rangeLoop && outside) {
        start = rate > 0 ? double(m_rangeStart) : double(m_rangeEnd - 1);
    } else if (!m_rangeLoop && rate > 0 && start >= double(m_rangeEnd - 1)) {
        start = double(m_rangeStart); // play at the end: start over (common NLE behaviour)
    }

    m_rate = rate;
    const bool useAudio = rate == 1.0 && hasAudio() && !m_muted;
    if (useAudio) {
        startAudioClock(start);
    } else {
        m_clock.startFree(start, rate, now);
    }
    m_motionStartUs = now;
    m_lastQualityChangeUs = now;
    m_droppedAtQualityCheck = m_stats.dropped;
    updateParams(false);
    setPositionInternal(int64_t(start), true);
    ensureTicking();
}

void PlaybackEngine::stopMotion()
{
    const int64_t now = monotonicUs();
    if (m_motionStartUs > 0) {
        m_stats.playSeconds += double(now - m_motionStartUs) * 1e-6;
        m_motionStartUs = 0;
    }
    stopAudio();
    m_clock.stop();
    m_scheduler.clear();
    m_playInOut = false;
    m_hasShownKey = false;
    updateParams(false);
    m_lastPlayheadWriteUs = 0;
    writePlayhead();
    // Show exactly the paused frame (playback may have been showing a lower quality or an
    // earlier frame when the renderer was behind).
    requestStill(true);
}

void PlaybackEngine::startAudioClock(double fromFrame)
{
    const int64_t startSample = framesToSamples48k(int64_t(fromFrame), fps());
    SampleLoop loop;
    if (m_rangeLoop) {
        loop.start = framesToSamples48k(m_rangeStart, fps());
        loop.end = framesToSamples48k(m_rangeEnd, fps());
    }
    m_feeder->start(m_snapshot, startSample, loop);
    // A short pre-roll so the device does not start with an underrun.
    (void)m_feeder->waitForFill(kPrerollFrames, kPrerollTimeoutMs);
    if (!m_audio->start(m_ring.get())) {
        qCWarning(lcPlayback) << "audio output failed to start; using the wall clock";
        m_feeder->stop();
        m_ring->clear();
        m_clock.startFree(fromFrame, 1.0, monotonicUs());
        return;
    }
    m_audioStartUs = monotonicUs();
    m_clock.startAudio(fromFrame, m_audioStartUs);
}

void PlaybackEngine::stopAudio()
{
    if (!m_audio) {
        return;
    }
    // Consumer first, then producer: afterwards nobody touches the ring and it may be cleared.
    m_audio->stop();
    m_feeder->stop();
    m_ring->clear();
}

void PlaybackEngine::requestStill(bool force)
{
    if (!m_snapshot) {
        return;
    }
    m_scheduler.requestStill(m_position, force);
    ensureTicking();
}

void PlaybackEngine::ensureTicking()
{
    if (!m_tick.isActive()) {
        m_tick.start();
    }
}

void PlaybackEngine::setPositionInternal(int64_t frame, bool writeBack)
{
    if (frame != m_position) {
        m_position = frame;
        Q_EMIT positionChanged(m_position);
    }
    if (writeBack) {
        if (isPlaying()) {
            // Throttled: the timeline view repaints on every playhead change.
            const int64_t now = monotonicUs();
            if (now - m_lastPlayheadWriteUs < int64_t(kPlayheadIntervalMs) * 1000) {
                return;
            }
            m_lastPlayheadWriteUs = now;
        }
        writePlayhead();
    }
}

void PlaybackEngine::writePlayhead()
{
    if (!m_timeline || m_timeline->playhead() == m_position) {
        return;
    }
    m_settingPlayhead = true;
    m_timeline->setPlayhead(m_position);
    m_settingPlayhead = false;
}

void PlaybackEngine::present(const RenderedFramePtr& frame, int64_t key)
{
    if (!frame || !frame->ok) {
        return;
    }
    if (isPlaying()) {
        ++m_stats.presented;
        if (std::abs(m_rate) == 1.0 && m_hasShownKey) {
            const int dir = m_rate > 0 ? 1 : -1;
            const int64_t gap = (key - m_lastShownKey) * dir - 1;
            if (gap > 0) {
                m_stats.dropped += gap;
            }
        }
        m_lastShownKey = key;
        m_hasShownKey = true;
    }
    m_current = frame;
    Q_EMIT frameReady(m_current);
}

void PlaybackEngine::updateAutoQuality(int64_t nowUs)
{
    if (m_qualityMode != QualityMode::Auto || !isPlaying()) {
        return;
    }
    // Let the render-time average refill after a change, then evaluate in fixed windows.
    if (nowUs - m_lastQualityChangeUs < kQualitySettleUs
        || nowUs - m_qualityEvalUs < kQualityEvalUs) {
        return;
    }
    m_qualityEvalUs = nowUs;
    const double avg = m_scheduler.averageRenderMs();
    const int64_t drops = m_stats.dropped - m_droppedAtQualityCheck;
    m_droppedAtQualityCheck = m_stats.dropped;
    if (avg <= 0.0) {
        return;
    }
    // Throughput needed: one frame per timeline frame interval whatever the rate (shuttle
    // shows every n-th frame at the same display rate).
    const double intervalMs = 1000.0 / std::max(1.0, fps().toDouble());
    const double load = avg / intervalMs;
    Quality next = m_autoQuality;
    if ((load > kDownshiftLoad || drops >= kDropsForDownshift)
        && m_autoQuality != Quality::Quarter) {
        next = lower(m_autoQuality);
    } else if (load < kUpshiftLoad && drops == 0 && m_autoQuality != Quality::Full) {
        if (m_headroomSinceUs < 0) {
            m_headroomSinceUs = nowUs;
        } else if (nowUs - m_headroomSinceUs >= int64_t(kUpshiftHoldMs) * 1000) {
            next = higher(m_autoQuality);
        }
    } else {
        m_headroomSinceUs = -1;
    }
    if (next == m_autoQuality) {
        return;
    }
    qCInfo(lcPlayback) << "auto quality" << qualityFactor(m_autoQuality) << "->"
                       << qualityFactor(next) << "render" << avg << "ms, drops" << drops;
    m_autoQuality = next;
    m_lastQualityChangeUs = nowUs;
    m_headroomSinceUs = -1;
    m_scheduler.resetAverage();
    updateParams(false);
}

void PlaybackEngine::onTick()
{
    const int64_t now = monotonicUs();
    if (!isPlaying()) {
        if (RenderedFramePtr frame = m_scheduler.pollStill()) {
            present(frame, frame->frame);
        }
        if (!m_scheduler.busy()) {
            m_tick.stop();
        }
        return;
    }

    int64_t played = -1;
    int64_t playedAt = -1;
    if (m_clock.isAudioMaster()) {
        m_audio->playedFrames(&played, &playedAt);
        if (played <= 0 && now - m_audioStartUs > int64_t(kAudioStartTimeoutMs) * 1000) {
            qCWarning(lcPlayback) << "audio device reports no progress; using the wall clock";
            const double pos = m_clock.lastPosition();
            stopAudio();
            m_clock.startFree(pos, 1.0, now);
            played = -1;
        }
    }
    const double unwrapped = m_clock.position(now, played, playedAt);
    const int dir = m_rate > 0 ? 1 : -1;
    int64_t dueKey = int64_t(std::floor(unwrapped));
    bool finished = false;
    if (!m_rangeLoop) {
        if (dir > 0 && dueKey >= m_rangeEnd) {
            dueKey = m_rangeEnd - 1;
            finished = true;
        } else if (dir < 0 && dueKey < m_rangeStart) {
            dueKey = m_rangeStart;
            finished = true;
        }
    }

    // Look-ahead: the wanted due at the next display intervals (one timeline frame interval
    // of real time apart). When rendering takes longer than an interval, start further ahead
    // so requests finish around their due time instead of after it.
    const double intervalMs = 1000.0 / std::max(1.0, fps().toDouble());
    const double avgMs = m_scheduler.averageRenderMs();
    const int lead = avgMs > 0.5 * intervalMs
        ? std::min(10, int(std::ceil(avgMs * m_scheduler.maxInFlight() / intervalMs)))
        : 0;
    std::vector<FrameSlot> wanted;
    wanted.reserve(size_t(m_scheduler.maxInFlight()) + 2);
    int64_t lastKey = INT64_MIN;
    for (int k = lead; k <= lead + m_scheduler.maxInFlight() + 1; ++k) {
        const int64_t key = int64_t(std::floor(unwrapped + m_rate * k));
        if (key == lastKey) {
            continue; // slow play: the same frame for several intervals
        }
        lastKey = key;
        if (!m_rangeLoop && (key >= m_rangeEnd || key < m_rangeStart)) {
            break;
        }
        if (m_hasShownKey && (key - m_lastShownKey) * dir <= 0) {
            continue; // already on screen (or older): never render a frame twice
        }
        wanted.push_back({key, int64_t(wrap(double(key)))});
    }
    m_scheduler.prefetch(wanted);

    int64_t shownKey = 0;
    if (RenderedFramePtr frame = m_scheduler.poll(dueKey, dir, &shownKey)) {
        present(frame, shownKey);
    }
    setPositionInternal(int64_t(wrap(double(dueKey))), true);
    updateAutoQuality(now);

    if (finished) {
        setRateInternal(0.0);
    }
}

} // namespace kk
