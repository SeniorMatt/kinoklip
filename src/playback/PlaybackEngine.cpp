// SPDX-License-Identifier: GPL-3.0-or-later
// PlaybackEngine — transport state machine, clocks, look-ahead and presentation.
#include "playback/PlaybackEngine.h"

#include "core/MediaPool.h"
#include "core/Timeline.h"
#include "playback/AudioFeeder.h"
#include "playback/AudioOutput.h"
#include "playback/AudioRing.h"
#include "playback/PlaybackLog.h"
#include "render/AudioMixer.h"
#include "render/Compositor.h"
#include "render/ProxySelection.h"
#include "render/TimelineEval.h"

#include <QSettings>

#include <algorithm>
#include <cmath>
#include <utility>

namespace kk {

namespace {

/// Ring between mixer thread and device: 8192 frames = 171 ms at 48 kHz.
constexpr size_t kRingFrames = 8192;
/// Pre-roll before the device starts (frames in the ring), and how long to wait for it.
constexpr size_t kPrerollFrames = 2 * 1024;
constexpr int kPrerollTimeoutMs = 60;
/// Longest a play from rest waits for its first frame before the clock starts anyway
/// (opening a hardware decoder for the first time takes 200 - 400 ms here).
constexpr int kVideoPrerollMaxMs = 800;
int64_t nowMs()
{
    return monotonicUs() / 1000;
}

} // namespace

PlaybackBehaviour PlaybackBehaviour::fromSettings()
{
    const QSettings settings;
    PlaybackBehaviour b;
    b.restartAtEnd
        = settings.value(QStringLiteral("playback/restartAtEnd"), b.restartAtEnd).toBool();
    b.prerollMs = settings.value(QStringLiteral("playback/prerollMs"), b.prerollMs).toInt();
    b.postrollMs = settings.value(QStringLiteral("playback/postrollMs"), b.postrollMs).toInt();
    b.returnAfterPlayAround
        = settings.value(QStringLiteral("playback/returnAfterPlayAround"), b.returnAfterPlayAround)
              .toBool();
    b.stopOnPlayheadMove
        = settings.value(QStringLiteral("playback/stopOnPlayheadMove"), b.stopOnPlayheadMove)
              .toBool();
    b.stopOnEdit = settings.value(QStringLiteral("playback/stopOnEdit"), b.stopOnEdit).toBool();
    return b;
}

PlaybackEngine::PlaybackEngine(QObject* parent)
    : PlaybackEngine(Options {}, parent)
{ }

namespace {

/// KK_PLAYBACK_STATS: any value except empty/0/false/off/no turns statistics on (so "1",
/// "yes", "true" all work). Read once; the first engine says so in the log.
bool statsEnabled()
{
    static const bool enabled = [] {
        const QByteArray v = qgetenv("KK_PLAYBACK_STATS").trimmed().toLower();
        const bool on = !v.isEmpty() && v != "0" && v != "false" && v != "off" && v != "no";
        if (on) {
            qCInfo(lcPlayback) << "KK_PLAYBACK_STATS: one stats line per second while playing "
                                  "and a summary when playback stops";
        }
        return on;
    }();
    return enabled;
}

} // namespace

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
    m_logStats = statsEnabled();
    m_tick.setTimerType(Qt::PreciseTimer);
    m_tick.setInterval(kTickMs);
    connect(&m_tick, &QTimer::timeout, this, &PlaybackEngine::onTick);
    m_shuttleTimer.setSingleShot(true);
    m_shuttleTimer.setTimerType(Qt::PreciseTimer);
    connect(
        &m_shuttleTimer, &QTimer::timeout, this, [this] { applyShuttle(m_shuttle.poll(nowMs())); });
    m_scrubIdle.setSingleShot(true);
    m_scrubIdle.setInterval(kScrubIdleMs);
    connect(&m_scrubIdle, &QTimer::timeout, this, [this] {
        if (m_scrubActive && !isPlaying())
            stopAudio();
    });
}

PlaybackEngine::~PlaybackEngine()
{
    if (m_logStats && isPlaying()) {
        logStatsSummary(); // quitting while playing
    }
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
    m_videoSnapshot.reset();
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
    m_poolChanged = connect(m_pool, &MediaPool::changed, this, [this] {
        setMediaPaths(m_audioOverride ? m_audioOverride : makeMediaPathMap(m_pool->state()));
    });
    setMediaPaths(m_audioOverride ? m_audioOverride : makeMediaPathMap(m_pool->state()));
}

void PlaybackEngine::setSubstitutes(
    std::shared_ptr<const ProxyMap> proxies, std::shared_ptr<const MediaPathMap> media)
{
    const bool proxiesChanged = proxies != m_proxies;
    m_proxies = std::move(proxies);
    m_audioOverride = std::move(media);
    if (m_audioOverride && m_audioOverride != m_media) {
        setMediaPaths(m_audioOverride); // also updates the parameters
    } else if (!m_audioOverride && m_pool) {
        setMediaPaths(makeMediaPathMap(m_pool->state()));
    } else if (proxiesChanged) {
        updateParams(true);
    }
}

void PlaybackEngine::setRenderCache(std::shared_ptr<const RenderCacheView> view)
{
    if (view == m_renderCache)
        return;
    m_renderCache = std::move(view);
    updateParams(false);
}

void PlaybackEngine::setBypassGrades(bool bypass)
{
    if (bypass == m_bypassGrades)
        return;
    m_bypassGrades = bypass;
    updateParams(true);
    Q_EMIT bypassGradesChanged(bypass);
}

void PlaybackEngine::setDynamicZoomApplied(bool applied)
{
    if (applied == m_dynamicZoomApplied)
        return;
    m_dynamicZoomApplied = applied;
    updateParams(true);
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
    std::shared_ptr<const TimelineData> next = m_timeline ? m_timeline->snapshot() : nullptr;
    // A sound-only edit (fader, EQ, automation, clip volume...) goes to the mixer only: the
    // pictures are the same, so the renderer keeps the previous snapshot and nothing in
    // flight is dropped.
    const bool soundOnly
        = next && m_snapshot && m_videoSnapshot && render::samePictures(*m_videoSnapshot, *next);
    m_snapshot = std::move(next);
    m_clock.setFps(fps());
    if (m_feeder && m_snapshot) {
        m_feeder->setTimeline(m_snapshot);
    }
    if (!isPlaying())
        m_committed = m_snapshot;
    if (!soundOnly) {
        m_videoSnapshot = m_snapshot;
        if (!m_snapshot) {
            setRateInternal(0.0);
            m_scheduler.clear();
            m_current.reset();
            Q_EMIT frameReady(m_current);
            return;
        }
        updateParams(true);
    } else if (!isPlaying()) {
        // Paused nothing is in flight to protect: render from the current snapshot (the render
        // cache view only serves requests for exactly its snapshot).
        m_videoSnapshot = m_snapshot;
        RenderParams params = m_scheduler.params();
        params.timeline = m_videoSnapshot;
        m_scheduler.setParams(params, false);
    }
    followRange();
}

void PlaybackEngine::followRange()
{
    if (!isPlaying() || !m_snapshot) {
        return;
    }
    int64_t start = 0;
    int64_t end = 0;
    range(&start, &end);
    if (start == m_rangeStart && end == m_rangeEnd) {
        return;
    }
    if (!m_rangeLoop || m_parked) {
        // Without a loop the clock never reads the range. (Parked: the resume restarts anyway.)
        m_rangeStart = start;
        m_rangeEnd = end;
        return;
    }
    // The clock's position wraps in the old loop and the feeder mixed it ahead: start again from
    // the frame shown (a short stop of the sound, only when the loop itself changes).
    m_resuming = true;
    startMotion(m_rate, double(m_position));
    m_resuming = false;
}

void PlaybackEngine::noteEditCommitted(bool keepsPlaying)
{
    const std::shared_ptr<const TimelineData> before = std::exchange(m_committed, m_snapshot);
    if (keepsPlaying || !isPlaying() || !before || !m_snapshot
        || render::sameArrangement(*before, *m_snapshot)) {
        return;
    }
    if (m_behaviour.stopOnEdit) {
        stop(); // shows the new timeline at the current frame (stopMotion())
        return;
    }
    if (!m_feeder || !m_clock.isAudioMaster() || m_parked || m_prerollUntilUs > 0) {
        return; // no sound running: it starts from the new timeline
    }
    // Only what the ring holds is out of date (edits further away need no remix).
    const auto ringFrames = int64_t(std::ceil(double(m_ring->capacity()) / 48000.0
                                * fps().toDouble() * std::abs(m_rate)))
        + 2;
    int64_t from = m_rate > 0 ? m_position - 1 : m_position - ringFrames;
    int64_t to = m_rate > 0 ? m_position + ringFrames : m_position + 2;
    if (m_rangeLoop && (to > m_rangeEnd || from < m_rangeStart)) {
        from = m_rangeStart; // the ring holds both ends of the loop
        to = m_rangeEnd;
    }
    if (render::soundArrangementDiffers(*before, *m_snapshot, from, to)) {
        m_feeder->remixUnheard();
    }
}

void PlaybackEngine::onPlayheadChanged(int64_t frame)
{
    if (m_settingPlayhead) {
        return; // our own write-back
    }
    // Someone else moved the playhead (timeline scrub, Home/End, edit navigation).
    if (isPlaying()) {
        seekWhilePlaying(frame, false);
        return;
    }
    m_position = frame;
    Q_EMIT positionChanged(m_position);
    requestStill(false);
    scrubAudio(frame);
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
    if (m_customRange) {
        *start = m_customStart;
        *end = std::min(m_customEnd, *end); // an edit may have shortened the timeline
    } else if (m_loop && m_snapshot) {
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

QSize PlaybackEngine::renderSize() const
{
    const QSize out = outputSize();
    const Quality quality = currentQuality();
    if (quality == Quality::Full) {
        return out;
    }
    // Proxy resolution: a fraction of the timeline resolution, never more than the viewer shows.
    const QSize native
        = m_snapshot ? QSize(m_snapshot->width, m_snapshot->height) : QSize(1920, 1080);
    const QSize proxy = scaledRenderSize(native, quality);
    return proxy.width() < out.width() ? proxy : out;
}

void PlaybackEngine::setQualityMode(QualityMode mode)
{
    if (mode == m_qualityMode) {
        return;
    }
    m_qualityMode = mode;
    m_auto.reset(monotonicUs());
    m_scheduler.resetAverage();
    updateParams(true);
}

std::future<RenderedFramePtr> PlaybackEngine::renderCompositionNode(
    std::shared_ptr<const TimelineData> timeline, quint64 clipId, quint64 nodeId, int64_t compFrame,
    QSize outSize, unsigned outputs) const
{
    return m_compositor->renderCompositionNode(std::move(timeline), clipId, nodeId, compFrame,
        outSize, currentQuality(), outputs, m_media, m_proxies);
}

std::future<RenderedFramePtr> PlaybackEngine::renderImage(
    int64_t frame, QSize outSize, ImageGrades grades) const
{
    RenderRequest request;
    request.timeline = m_videoSnapshot ? m_videoSnapshot : m_snapshot;
    if (grades == ImageGrades::ClipsOnly && request.timeline
        && !request.timeline->grade.isEmpty()) {
        auto copy = std::make_shared<TimelineData>(*request.timeline);
        copy->grade = ClipGrade {};
        request.timeline = std::move(copy);
    }
    request.media = m_media;
    request.proxies = m_proxies;
    request.renderCache = m_renderCache;
    request.bypassGrades
        = grades == ImageGrades::None || (grades == ImageGrades::Viewer && m_bypassGrades);
    request.dynamicZoom = m_dynamicZoomApplied;
    request.frame = frame;
    request.outSize = outSize;
    request.outputs = OutputImage;
    request.subtitles = false;
    return m_compositor->renderAsync(std::move(request));
}

std::future<RenderedFramePtr> PlaybackEngine::renderAlternative(
    std::shared_ptr<const TimelineData> timeline, int64_t frame,
    std::shared_ptr<const std::atomic<bool>> cancelled) const
{
    const RenderParams& params = m_scheduler.params();
    RenderRequest request;
    request.timeline = std::move(timeline);
    request.media = m_media;
    request.proxies = m_proxies;
    request.bypassGrades = m_bypassGrades;
    request.dynamicZoom = m_dynamicZoomApplied;
    request.frame = frame;
    request.outSize = params.outSize;
    request.quality = currentQuality();
    request.outputs = params.outputs;
    request.cancelled = std::move(cancelled);
    return m_compositor->renderAsync(std::move(request));
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
    return isPlaying() ? m_auto.quality() : Quality::Full;
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
    params.timeline = m_videoSnapshot ? m_videoSnapshot : m_snapshot;
    params.media = m_media;
    params.proxies = m_proxies;
    params.renderCache = m_renderCache;
    params.bypassGrades = m_bypassGrades;
    params.dynamicZoom = m_dynamicZoomApplied;
    // The proxy resolution is applied here (relative to the timeline resolution, see
    // renderSize()); the compositor renders exactly that size.
    params.outSize = renderSize();
    params.quality = Quality::Full;
    const Quality quality = currentQuality();
    const bool allTextures = !m_clients.isEmpty()
        && std::all_of(
            m_clients.cbegin(), m_clients.cend(), [](const Client& c) { return c.textures; });
    params.outputs = m_compositor->isGpu() && allTextures ? OutputTexture : OutputImage;
    const RenderParams& old = m_scheduler.params();
    const bool contentChanged = old.timeline != params.timeline || old.media != params.media
        || old.proxies != params.proxies || old.bypassGrades != params.bypassGrades
        || old.dynamicZoom != params.dynamicZoom;
    const bool changed
        = contentChanged || old.outSize != params.outSize || old.outputs != params.outputs;
    if (old.timeline != params.timeline || old.proxies != params.proxies) {
        // Viewer indicator: does this timeline show any substitute? (one pass over its clips)
        const bool inUse = params.timeline && params.proxies
            && usesSubstitutes(*params.timeline, *params.proxies);
        if (inUse != m_proxiesInUse) {
            m_proxiesInUse = inUse;
            Q_EMIT proxiesInUseChanged(inUse);
        }
    }
    if (contentChanged) {
        m_currentStale = true; // the frame on screen shows the old pictures
        if (isPlaying()) {
            m_hasShownKey = false; // the frame due now is wanted again, in its new form
            m_wantDueFrame = true;
        }
    }
    if (changed) {
        // Size/quality changes during playback keep the frames in flight (same picture).
        m_scheduler.setParams(params, contentChanged || !isPlaying());
        if (contentChanged && m_parked) {
            m_parkShown = false; // the seek target as well
            m_scheduler.requestStill(m_parkTarget, true);
        }
    } else if (old.renderCache != params.renderCache) {
        // A new cache view shows the same pictures: later requests use it, nothing in flight
        // or on screen needs to be redone.
        m_scheduler.setParams(params, false);
    }
    if (quality != m_lastEmittedQuality) {
        m_lastEmittedQuality = quality;
        Q_EMIT qualityChanged(quality);
    }
    if (rerender && changed && !isPlaying() && m_snapshot) {
        requestStill(true);
    }
}

// ---- Meters ----

void PlaybackEngine::resetLoudness()
{
    if (m_feeder)
        m_feeder->resetLoudness();
    m_meterFrame.integratedLufs = -200.0;
    m_meterFrame.truePeakMaxDb = -200.0;
    Q_EMIT metersUpdated();
}

void PlaybackEngine::updateMeters(int64_t nowUs, double heardFrame)
{
    if (!m_feeder || !m_clock.isAudioMaster()
        || nowUs - m_lastMeterUs < int64_t(kMeterIntervalMs) * 1000)
        return;
    m_lastMeterUs = nowUs;
    const int64_t sample = int64_t(heardFrame * AudioMixer::kSampleRate * fps().den / fps().num);
    if (m_feeder->meterFrameAt(sample, &m_meterFrame))
        Q_EMIT metersUpdated();
}

void PlaybackEngine::clearMeters()
{
    if (m_feeder)
        m_feeder->clearMeters();
    if (m_meterFrame.sample < 0)
        return;
    const double integrated = m_meterFrame.integratedLufs;
    const double truePeakMax = m_meterFrame.truePeakMaxDb;
    m_meterFrame = MeterFrame {};
    // The program's integrated loudness stays on display until it is reset (Resolve).
    m_meterFrame.integratedLufs = integrated;
    m_meterFrame.truePeakMaxDb = truePeakMax;
    Q_EMIT metersUpdated();
}

// ---- Statistics ----

PlaybackEngine::Stats PlaybackEngine::stats() const
{
    Stats s = m_stats;
    if (isPlaying() && m_motionStartUs > 0) {
        s.playSeconds += double(monotonicUs() - m_motionStartUs) * 1e-6;
    }
    s.averageRenderMs = m_scheduler.averageRenderMs();
    if (m_stageCount > 0) {
        s.averageDecodeMs = m_stageSum[0] / double(m_stageCount);
        s.averageUploadMs = m_stageSum[1] / double(m_stageCount);
        s.averageCompositeMs = m_stageSum[2] / double(m_stageCount);
        s.averageOutputMs = m_stageSum[3] / double(m_stageCount);
    }
    s.audioMaster = m_clock.isAudioMaster() && isPlaying();
    s.avOffsetFrames = m_clock.lastAudioError();
    return s;
}

void PlaybackEngine::resetStats()
{
    m_stats = Stats {};
    std::fill(std::begin(m_stageSum), std::end(m_stageSum), 0.0);
    m_stageCount = 0;
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
    m_customRange = false;
    m_returnTo = -1;
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
        seekWhilePlaying(frame, true);
        return;
    }
    setPositionInternal(frame, true);
    requestStill(false);
    scrubAudio(frame);
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
    m_customRange = true;
    m_customStart = m_snapshot->markIn >= 0 ? m_snapshot->markIn : 0;
    m_customEnd = m_snapshot->markOut > m_customStart ? m_snapshot->markOut : duration();
    m_returnTo = -1;
    setPositionInternal(m_customStart, true);
    setRateInternal(1.0);
}

void PlaybackEngine::playAroundCurrent()
{
    if (!m_snapshot) {
        return;
    }
    if (isPlaying()) {
        setRateInternal(0.0);
    }
    const int64_t here = m_position;
    const double fpsValue = fps().toDouble();
    const int64_t pre = int64_t(std::llround(m_behaviour.prerollMs * fpsValue / 1000.0));
    const int64_t post = int64_t(std::llround(m_behaviour.postrollMs * fpsValue / 1000.0));
    m_customRange = true;
    m_customStart = std::max<int64_t>(0, here - pre);
    m_customEnd = std::max(m_customStart + 1, std::min(duration(), here + post + 1));
    m_returnTo = m_behaviour.returnAfterPlayAround ? here : -1;
    setPositionInternal(m_customStart, true);
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
    // A new play (not a seek, rate change or resume after a seek).
    const bool fromRest = m_motionStartUs == 0 && !m_parked;
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
    } else if (!m_rangeLoop && rate > 0 && start >= double(m_rangeEnd - 1)
        && m_behaviour.restartAtEnd && !m_resuming) {
        start = double(m_rangeStart); // play on the last frame: start over (PlaybackBehaviour)
    }

    m_rate = rate;
    m_statsPlayRate = rate;
    m_prerollUntilUs = 0;
    // A play from rest whose start frame is not on screen yet (the decoders are still
    // opening): hold the clock until it is (onTick()), so no frames are dropped at the start.
    const bool startShown
        = m_current && m_current->ok && !m_currentStale && m_current->frame == int64_t(start);
    if (fromRest && !startShown) {
        m_prerollUntilUs = now + int64_t(kVideoPrerollMaxMs) * 1000;
        m_prerollFrom = start;
    } else {
        startClock(start, now);
    }
    m_motionStartUs = now;
    if (fromRest) {
        m_auto.reset(now);
        m_scheduler.resetAverage(); // paused stills say nothing about playback
        if (m_logStats) {
            m_statsAtPlay = stats();
            std::copy(std::begin(m_stageSum), std::end(m_stageSum), m_playStageSumAtStart);
            m_playStageCountAtStart = m_stageCount;
            m_playStatsActive = true;
        }
    }
    m_parked = false;
    updateParams(false);
    setPositionInternal(int64_t(start), true);
    // The picture on screen (the paused frame, a seek target) need not be rendered again,
    // unless an edit made it outdated (it may show a clip that is no longer there).
    if (m_current && !m_currentStale && m_current->frame == int64_t(start)) {
        m_lastShownKey = int64_t(start);
        m_hasShownKey = true;
    }
    ensureTicking();
}

void PlaybackEngine::startClock(double fromFrame, int64_t nowUs)
{
    // Every rate is heard (varispeed, pitched); see AudioFeeder.
    const bool useAudio = m_rate != 0.0 && hasAudio() && !m_muted;
    if (useAudio) {
        startAudioClock(fromFrame);
    } else {
        m_clock.startFree(fromFrame, m_rate, nowUs);
    }
}

void PlaybackEngine::seekWhilePlaying(int64_t frame, bool writePlayhead)
{
    frame = clampFrame(frame);
    if (m_behaviour.stopOnPlayheadMove) {
        // Preference (Premiere): the move ends playback; the new frame is the paused one.
        m_position = frame;
        stop(); // writes the playhead and shows exactly that frame (stopMotion())
        Q_EMIT positionChanged(m_position);
        return;
    }
    const int64_t now = monotonicUs();
    m_prerollUntilUs = 0; // a seek parks: resuming starts the clock
    ++m_stats.seeksWhilePlaying;
    if (!m_parked) {
        // Leave motion: stale look-ahead is cancelled, the sound stops until we resume.
        if (m_motionStartUs > 0) {
            m_stats.playSeconds += double(now - m_motionStartUs) * 1e-6;
            m_motionStartUs = 0;
        }
        stopAudio();
        m_clock.stop();
        m_scheduler.clear();
        m_hasShownKey = false;
        m_parked = true;
    }
    m_parkTarget = frame;
    m_lastSeekUs = now;
    m_parkShown = m_current && !m_currentStale && m_current->frame == frame;
    if (m_parkShown) {
        m_stats.lastSeekMs = 0.0;
    }
    if (frame != m_position) {
        m_position = frame;
        Q_EMIT positionChanged(m_position);
    }
    if (writePlayhead) {
        this->writePlayhead();
    }
    m_scheduler.requestStill(frame); // coalesced: one in flight, the newest wish pending
    ensureTicking();
}

void PlaybackEngine::resumeFromPark()
{
    m_resuming = true;
    startMotion(m_rate, double(m_parkTarget));
    m_resuming = false;
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
    m_parked = false;
    m_prerollUntilUs = 0;
    if (m_logStats) {
        logStatsSummary(); // also covers plays shorter than one stats window (1 s)
    }
    m_statsLogUs = 0; // the next play starts a new statistics window
    m_customRange = false;
    m_hasShownKey = false;
    if (m_snapshot) {
        m_videoSnapshot = m_snapshot; // see onTimelineChanged(); a still is rendered below anyway
    }
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
    m_feeder->start(m_snapshot, startSample, loop, m_rate);
    // A short pre-roll so the device does not start with an underrun.
    (void)m_feeder->waitForFill(kPrerollFrames, kPrerollTimeoutMs);
    if (!m_audio->start(m_ring.get())) {
        qCWarning(lcPlayback) << "audio output failed to start; using the wall clock";
        m_feeder->stop();
        m_ring->clear();
        m_clock.startFree(fromFrame, m_rate, monotonicUs());
        return;
    }
    m_audioStartUs = monotonicUs();
    m_clock.startAudio(fromFrame, m_audioStartUs, m_rate);
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
    clearMeters();
    m_scrubActive = false;
    m_scrubIdle.stop();
}

namespace {
/// PlaybackEngine::setAudioScrubbing(): one switch for all engines (GUI thread only).
bool g_audioScrubbing = true;
} // namespace

void PlaybackEngine::setAudioScrubbing(bool on)
{
    g_audioScrubbing = on;
}

bool PlaybackEngine::audioScrubbing()
{
    return g_audioScrubbing;
}

namespace {
/// Live PlaybackEngine::SilentPlayheadMove scopes (GUI thread only).
int g_silentPlayheadMoves = 0;
} // namespace

PlaybackEngine::SilentPlayheadMove::SilentPlayheadMove()
{
    ++g_silentPlayheadMoves;
}

PlaybackEngine::SilentPlayheadMove::~SilentPlayheadMove()
{
    --g_silentPlayheadMoves;
}

bool PlaybackEngine::playheadMovesSilent()
{
    return g_silentPlayheadMoves > 0;
}

void PlaybackEngine::scrubAudio(int64_t frame)
{
    if (!g_audioScrubbing || g_silentPlayheadMoves > 0 || isPlaying() || m_muted || !hasAudio()
        || !m_snapshot)
        return;
    if (!m_scrubActive) {
        // Open the device once per scrub gesture; snippets then only feed the ring.
        m_feeder->startScrub(m_snapshot);
        if (!m_audio->start(m_ring.get())) {
            m_feeder->stop();
            m_ring->clear();
            return;
        }
        m_scrubActive = true;
    }
    m_feeder->scrubTo(framesToSamples48k(frame, fps()));
    m_scrubIdle.start();
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
    if (isPlaying() && !m_parked) {
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
        const double stages[4]
            = {frame->decodeMs, frame->uploadMs, frame->compositeMs, frame->outputMs};
        for (int i = 0; i < 4; ++i) {
            m_stageSum[i] += stages[i];
            m_winSum[i] += stages[i];
        }
        ++m_stageCount;
        ++m_winCount;
    }
    m_current = frame;
    m_currentStale = false;
    Q_EMIT frameReady(m_current);
}

void PlaybackEngine::updateAutoQuality(int64_t nowUs)
{
    if (m_qualityMode != QualityMode::Auto || !isPlaying()) {
        return;
    }
    // Throughput needed: one frame per timeline frame interval whatever the rate (shuttle
    // shows every n-th frame at the same display rate).
    const double intervalMs = 1000.0 / std::max(1.0, fps().toDouble());
    const Quality before = m_auto.quality();
    if (!m_auto.update(nowUs, m_scheduler.averageRenderMs(), intervalMs)) {
        return;
    }
    qCInfo(lcPlayback) << "auto quality" << qualityFactor(before) << "->"
                       << qualityFactor(m_auto.quality()) << "load" << m_auto.lastLoad();
    m_scheduler.resetAverage();
    updateParams(false);
}

void PlaybackEngine::logStats(int64_t nowUs)
{
    if (m_statsLogUs == 0) {
        m_statsLogUs = nowUs;
        m_statsAtLog = stats();
        return;
    }
    if (nowUs - m_statsLogUs < 1'000'000) {
        return;
    }
    const Stats s = stats();
    const double seconds = double(nowUs - m_statsLogUs) * 1e-6;
    const int64_t frames = std::max<int64_t>(1, m_winCount);
    auto stage = [&](int i) { return m_winSum[i] / double(frames); };
    const QSize size = renderSize();
    qCInfo(lcPlayback).noquote() << QStringLiteral(
        "stats: %1 fps (of %2), %3 dropped, quality %4 %5x%6, render %7 ms "
        "[decode %8, upload %9, composite %10, output %11], %12, A/V %13")
                                        .arg(double(s.presented - m_statsAtLog.presented) / seconds,
                                            0, 'f', 1)
                                        .arg(fps().toDouble() * std::abs(m_rate), 0, 'f', 2)
                                        .arg(s.dropped - m_statsAtLog.dropped)
                                        .arg(qualityFactor(currentQuality()))
                                        .arg(size.width())
                                        .arg(size.height())
                                        .arg(s.averageRenderMs, 0, 'f', 1)
                                        .arg(stage(0), 0, 'f', 1)
                                        .arg(stage(1), 0, 'f', 1)
                                        .arg(stage(2), 0, 'f', 1)
                                        .arg(stage(3), 0, 'f', 1)
                                        .arg(s.audioMaster ? QStringLiteral("audio clock")
                                                           : QStringLiteral("wall clock"))
                                        .arg(s.avOffsetFrames, 0, 'f', 2);
    m_statsLogUs = nowUs;
    m_statsAtLog = s;
    std::fill(std::begin(m_winSum), std::end(m_winSum), 0.0);
    m_winCount = 0;
}

void PlaybackEngine::logStatsSummary()
{
    if (!m_playStatsActive) {
        return;
    }
    m_playStatsActive = false;
    const render::DecoderPool::PrepareStats look = m_compositor->prepareStats();
    qCInfo(lcPlayback).noquote()
        << QStringLiteral("cut look-ahead (since the compositor started): %1 prepared, %2 ready "
                          "at the cut, %3 waited, %4 cancelled, %5 unused")
               .arg(look.started)
               .arg(look.hitsReady)
               .arg(look.hitsWaited)
               .arg(look.cancelled)
               .arg(look.expired);
    const Stats s = stats();
    const double seconds = s.playSeconds - m_statsAtPlay.playSeconds;
    const int64_t presented = s.presented - m_statsAtPlay.presented;
    const int64_t frames = std::max<int64_t>(1, m_stageCount - m_playStageCountAtStart);
    auto stage = [&](int i) { return (m_stageSum[i] - m_playStageSumAtStart[i]) / double(frames); };
    qCInfo(lcPlayback).noquote() << QStringLiteral(
        "stats summary: played %1 s, %2 frames shown (%3 fps of %4), %5 dropped, %6 seeks "
        "while playing, render avg [decode %7, upload %8, composite %9, output %10] ms")
                                        .arg(seconds, 0, 'f', 2)
                                        .arg(presented)
                                        .arg(seconds > 0 ? double(presented) / seconds : 0.0, 0,
                                            'f', 1)
                                        .arg(
                                            fps().toDouble() * std::abs(m_statsPlayRate), 0, 'f', 2)
                                        .arg(s.dropped - m_statsAtPlay.dropped)
                                        .arg(s.seeksWhilePlaying - m_statsAtPlay.seeksWhilePlaying)
                                        .arg(stage(0), 0, 'f', 1)
                                        .arg(stage(1), 0, 'f', 1)
                                        .arg(stage(2), 0, 'f', 1)
                                        .arg(stage(3), 0, 'f', 1);
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

    if (m_prerollUntilUs > 0) {
        // Video pre-roll (startMotion()): the first frames go through the playback path (so
        // its decoders open now, not while the clock runs); the clock starts once the first
        // one is ready, or at the deadline.
        const int dir = m_rate > 0 ? 1 : -1;
        const int64_t first = int64_t(m_prerollFrom);
        std::vector<FrameSlot> wanted;
        for (int k = 0; k <= m_scheduler.maxInFlight(); ++k) {
            const int64_t key = first + dir * k;
            if (!m_rangeLoop && (key >= m_rangeEnd || key < m_rangeStart))
                break;
            wanted.push_back({key, int64_t(wrap(double(key)))});
        }
        m_scheduler.prefetch(wanted);
        int64_t shownKey = 0;
        RenderedFramePtr frame = m_scheduler.poll(first, dir, &shownKey);
        const bool ready = frame && frame->ok;
        if (ready || now >= m_prerollUntilUs) {
            m_prerollUntilUs = 0;
            m_motionStartUs = now;
            if (ready)
                present(frame, shownKey); // the first played frame (nothing dropped before)
            startClock(m_prerollFrom, now);
        }
        return;
    }

    if (m_parked) {
        if (RenderedFramePtr frame = m_scheduler.pollStill()) {
            present(frame, frame->frame);
            if (frame->ok && frame->frame == m_parkTarget && !m_parkShown) {
                m_parkShown = true;
                m_stats.lastSeekMs = double(now - m_lastSeekUs) / 1000.0;
            }
        }
        const int64_t quietUs = now - m_lastSeekUs;
        if (quietUs >= int64_t(kSeekResumeMs) * 1000
            && (m_parkShown || quietUs >= int64_t(kSeekMaxParkMs) * 1000)) {
            resumeFromPark();
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
            m_clock.startFree(pos, m_rate, now);
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
    wanted.reserve(size_t(m_scheduler.maxInFlight()) + 3);
    int64_t lastKey = INT64_MIN;
    if (m_wantDueFrame) {
        // The pictures changed: the frame due now first, even though it will be late, so the new
        // timeline is on screen at once instead of at the far end of the look-ahead.
        wanted.push_back({dueKey, int64_t(wrap(double(dueKey)))});
    }
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
        if (!wanted.empty() && wanted.front().key == key) {
            continue;
        }
        wanted.push_back({key, int64_t(wrap(double(key)))});
    }
    if (m_scheduler.prefetch(wanted) > 0) {
        m_wantDueFrame = false;
    }
    // Forward at up to 2x, the decoders of the next clips get ready well before their cuts.
    // (In reverse every frame seeks anyway, and J/K/L at 4x+ lands anywhere after a cut.)
    // KK_NO_CUT_LOOKAHEAD: without it, to compare (tests/playback/test_cutperf).
    static const bool lookAhead = qEnvironmentVariableIsEmpty("KK_NO_CUT_LOOKAHEAD");
    if (lookAhead && m_rate > 0.0 && m_rate <= kCutLookAheadMaxRate) {
        int64_t lastKey = dueKey + int64_t(kCutLookAheadMs / intervalMs * m_rate);
        if (!m_rangeLoop)
            lastKey = std::min(lastKey, m_rangeEnd - 1);
        m_scheduler.prepareAhead(dueKey, lastKey, intervalMs / m_rate,
            [this](int64_t key) { return int64_t(wrap(double(key))); });
    }

    int64_t shownKey = 0;
    if (RenderedFramePtr frame = m_scheduler.poll(dueKey, dir, &shownKey)) {
        present(frame, shownKey);
    }
    setPositionInternal(int64_t(wrap(double(dueKey))), true);
    updateMeters(now, wrap(unwrapped));
    updateAutoQuality(now);
    if (m_logStats) {
        logStats(now);
    }

    if (finished) {
        const int64_t back = m_returnTo;
        m_returnTo = -1;
        setRateInternal(0.0);
        if (back >= 0) {
            seek(back); // play around current: back to where it started
        }
    }
}

} // namespace kk
