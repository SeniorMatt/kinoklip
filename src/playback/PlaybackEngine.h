// SPDX-License-Identifier: GPL-3.0-or-later
// PlaybackEngine — plays a Timeline: transport state machine (play/pause/J-K-L shuttle/step/
// loop), audio-master clock, frame look-ahead through the Compositor, preview quality.
#pragma once

#include "playback/AudioOutput.h"
#include "playback/AutoQuality.h"
#include "playback/FrameScheduler.h"
#include "playback/PlaybackClock.h"
#include "playback/ShuttleControl.h"
#include "render/RenderTypes.h"

#include <QHash>
#include <QObject>
#include <QSize>
#include <QTimer>

#include <cstdint>
#include <memory>

namespace kk {

class AudioFeeder;
class AudioRing;
class Compositor;
class MediaPool;
class Timeline;

/// Transport conventions that differ between editors (and future "interface profiles", see
/// plan.md 5.10). Kept as data so a profile can swap them; read from QSettings by
/// fromSettings() (keys under "playback/").
struct PlaybackBehaviour
{
    /// Play pressed on the last frame starts again from the start of the range (true), or
    /// does nothing (false).
    bool restartAtEnd = true;
    /// "Play around current frame": pre-roll / post-roll around the playhead (ms).
    int prerollMs = 2000;
    int postrollMs = 2000;
    /// After play-around (and play in-to-out), return the playhead to where it started.
    bool returnAfterPlayAround = true;

    /// Values stored in QSettings ("playback/restartAtEnd", "playback/prerollMs",
    /// "playback/postrollMs", "playback/returnAfterPlayAround"), defaults where unset.
    [[nodiscard]] static PlaybackBehaviour fromSettings();
};

/// Plays a kk::Timeline (the program, or the private one-clip timeline of a SourcePlayer).
///
/// **Transport.** stopped (paused) / playing (forward 1x) / shuttling (any other rate,
/// -8x..8x, including 0.5x slow play). J/K/L go through ShuttleControl. step() pauses and moves
/// by frames. Loop (toggle) repeats [in, out) if both marks are set, else the whole timeline;
/// playInToOut() plays the marked range once (or looping if loop is on).
///
/// **Clock.** Forward 1x with sound: the audio device is the master (PlaybackClock in audio
/// mode, fed by AudioOutput::playedFrames()); the AudioFeeder thread mixes ahead into a
/// lock-free AudioRing that the device pulls from. Reverse, shuttle, slow, muted or no
/// device: a free-running wall clock. If the device produces no position within
/// kAudioStartTimeoutMs the engine falls back to the wall clock (logged).
///
/// **Video.** A precise ~4 ms GUI timer polls the clock; FrameScheduler keeps 2-3
/// Compositor::renderAsync() requests ahead of it and presents the newest finished frame that
/// is due (frameReady()). Late frames are dropped, never shown late, so picture and sound
/// never drift apart. While paused, playhead moves (scrub) and timeline edits render one
/// coalesced still frame at Full quality.
///
/// **Quality** (Resolve's "Timeline Proxy Resolution"). Full (the default) renders every frame
/// at full resolution and drops frames when rendering cannot keep up. Half / Quarter cap the
/// render resolution at 1/2 or 1/4 of the *timeline* resolution. Auto (opt-in) renders Full
/// while paused and lets AutoQuality switch between Full and Half during playback.
///
/// **Output size.** Viewers register as output clients (setOutputClient()) with their picture
/// size in device pixels; outputSize() is the timeline aspect fitted into the largest client,
/// never above timeline resolution, and frames are rendered at renderSize() = the smaller of
/// outputSize() and the proxy resolution (so Half changes nothing for a viewer that shows
/// fewer pixels than half the timeline resolution anyway). Textures (zero copy) are requested
/// when the GPU compositor is active and every client can draw them, otherwise images.
///
/// **Seeking while playing** (clicking or dragging the ruler or playhead, Home/End...): playback
/// carries on from the new position, like Resolve. The engine *parks*: the stale look-ahead is
/// cancelled, sound stops, the target frame is rendered like a paused scrub (coalesced, so a
/// drag renders what it can and always ends on the last position), and clock and sound resume
/// from the target once it is on screen and no further seek arrived for kSeekResumeMs (or
/// after kSeekMaxParkMs at the latest). The transport state stays Playing throughout.
///
/// **Timeline sync.** The engine follows Timeline::playheadChanged (scrub/seek from the
/// timeline, keyboard navigation) and writes the playhead back at most every
/// kPlayheadIntervalMs during playback (the timeline view follows it). Timeline::changed
/// updates the snapshot used for new requests and the audio mix.
///
/// Thread-safety: GUI thread only. Owns the audio feeder thread and (if not given one) the
/// Compositor with its render thread.
class PlaybackEngine : public QObject
{
    Q_OBJECT
public:
    /// Transport state.
    enum class State {
        Stopped, ///< not moving (paused)
        Playing, ///< forward at 1x
        Shuttling ///< any other non-zero rate
    };
    Q_ENUM(State)

    /// Timeline proxy resolution setting (the values are stored in settings: keep the order).
    enum class QualityMode {
        Auto,
        Full,
        Half,
        Quarter
    };
    Q_ENUM(QualityMode)

    /// Construction options (all optional).
    struct Options
    {
        /// Compositor to share (e.g. the program engine's); nullptr = create one.
        std::shared_ptr<Compositor> compositor;
        /// Sound device; nullptr = the default QAudioSink output (unless noAudio).
        std::unique_ptr<AudioOutput> audio;
        /// No sound device at all (always the free-running clock).
        bool noAudio = false;
    };

    /// Playback statistics since the last resetStats() (measurements, tests).
    struct Stats
    {
        int64_t presented = 0; ///< frames shown while playing
        int64_t dropped = 0; ///< frames skipped at |rate| = 1 (gaps in the presented sequence)
        double playSeconds = 0.0; ///< wall time spent playing
        double averageRenderMs = 0.0; ///< compositor time per presented frame (moving average)
        double averageDecodeMs = 0.0; ///< of averageRenderMs: decoding (+ hw download)
        double averageUploadMs = 0.0; ///< ... texture uploads
        double averageCompositeMs = 0.0; ///< ... drawing (excluding uploads)
        double averageOutputMs = 0.0; ///< ... output texture / readback
        double lastSeekMs = 0.0; ///< last seek while playing: until its target was on screen
        int64_t seeksWhilePlaying = 0; ///< seeks while playing (each move of a drag counts)
        bool audioMaster = false; ///< current clock source
        double avOffsetFrames = 0.0; ///< last audio - clock error seen by the clock (frames)
        [[nodiscard]] double fps() const { return playSeconds > 0 ? presented / playSeconds : 0.0; }
    };

    static constexpr int kTickMs = 4; ///< clock poll / presentation interval
    static constexpr int kPlayheadIntervalMs = 33; ///< Timeline::setPlayhead throttle (~30 Hz)
    static constexpr int kAudioStartTimeoutMs = 600; ///< no device position -> wall clock
    static constexpr int kSeekResumeMs = 80; ///< quiet time after a seek before motion resumes
    static constexpr int kSeekMaxParkMs = 400; ///< resume even if the target is not shown yet

    /// Creates an engine with its own Compositor and the default audio output.
    explicit PlaybackEngine(QObject* parent = nullptr);
    /// Creates an engine with @p options.
    explicit PlaybackEngine(Options options, QObject* parent = nullptr);
    ~PlaybackEngine() override;

    // ---- Content ----

    /// Plays @p timeline (not owned; nullptr = nothing). Stops playback.
    void setTimeline(Timeline* timeline);
    [[nodiscard]] Timeline* timeline() const { return m_timeline; }
    /// Media paths follow @p pool (not owned) from now on.
    void setMediaPool(MediaPool* pool);
    /// Fixed media path table (instead of setMediaPool()).
    void setMediaPaths(std::shared_ptr<const MediaPathMap> media);
    /// Proxy / optimized media for the picture (render/ProxySelection.h, nullptr = the camera
    /// originals) and the media table to use instead of the pool's own paths (offline
    /// originals play their proxy's sound; nullptr = the pool's paths). The application
    /// rebuilds both when the pool, the links or Playback > Proxy Handling change.
    void setSubstitutes(
        std::shared_ptr<const ProxyMap> proxies, std::shared_ptr<const MediaPathMap> media);
    /// Proxy map in use (viewer indicator).
    [[nodiscard]] const std::shared_ptr<const ProxyMap>& proxies() const { return m_proxies; }
    /// True when the current timeline shows at least one proxy / optimized media file.
    [[nodiscard]] bool proxiesInUse() const { return m_proxiesInUse; }
    /// Render cache of the current timeline snapshot (Playback > Render Cache); frames of
    /// cached chunks are shown from the cache. nullptr = no cache.
    void setRenderCache(std::shared_ptr<const RenderCacheView> view);
    /// The compositor (shareable with other engines).
    [[nodiscard]] std::shared_ptr<Compositor> compositor() const { return m_compositor; }

    // ---- State ----

    [[nodiscard]] State state() const;
    /// Current rate (0 = paused; 1 = play; negative = reverse).
    [[nodiscard]] double rate() const { return m_rate; }
    [[nodiscard]] bool isPlaying() const { return m_rate != 0.0; }
    /// Current position in timeline frames (the clock's frame while playing).
    [[nodiscard]] int64_t position() const { return m_position; }
    /// Length of the timeline in frames (0 if none).
    [[nodiscard]] int64_t duration() const;
    [[nodiscard]] bool loop() const { return m_loop; }
    /// Transport conventions (restart at end, pre/post-roll).
    void setBehaviour(const PlaybackBehaviour& behaviour) { m_behaviour = behaviour; }
    [[nodiscard]] const PlaybackBehaviour& behaviour() const { return m_behaviour; }
    /// The last presented frame (keep it alive while drawing; see RenderedFrame).
    [[nodiscard]] RenderedFramePtr currentFrame() const { return m_current; }

    // ---- Output ----

    /// Registers/updates viewer @p client wanting pictures of up to @p pixelSize (device
    /// pixels); @p canDrawTextures: the client draws GL textures (zero copy).
    void setOutputClient(const void* client, QSize pixelSize, bool canDrawTextures);
    /// Unregisters @p client.
    void removeOutputClient(const void* client);
    /// Picture size of the largest client (timeline aspect, never above timeline resolution).
    [[nodiscard]] QSize outputSize() const;
    /// Size frames are rendered at now: outputSize() capped by the proxy resolution.
    [[nodiscard]] QSize renderSize() const;
    void setQualityMode(QualityMode mode);
    [[nodiscard]] QualityMode qualityMode() const { return m_qualityMode; }
    /// Quality used for the next requests.
    [[nodiscard]] Quality currentQuality() const;
    /// Mutes the sound (the clock becomes the wall clock).
    void setMuted(bool muted);
    [[nodiscard]] bool isMuted() const { return m_muted; }
    /// Linear output volume 0..1.
    void setVolume(float volume);
    /// True if a sound device is available.
    [[nodiscard]] bool hasAudio() const;

    // ---- Statistics ----
    [[nodiscard]] Stats stats() const;
    void resetStats();
    /// The frame scheduler (tests).
    [[nodiscard]] FrameScheduler& scheduler() { return m_scheduler; }

public Q_SLOTS:
    /// Plays forward at 1x from the current position (from the start if at the end).
    void play();
    /// Pauses at the current frame.
    void pause();
    /// Pauses if moving, plays otherwise.
    void togglePlay();
    /// Stops: like pause(), and also ends play-in-to-out and resets the shuttle.
    void stop();
    /// Plays at @p rate (clamped to +/-ShuttleControl::kMaxRate; 0 pauses).
    void setRate(double rate);
    /// L: forward shuttle (1x, 2x, 4x, 8x) or, with K held, frame step / slow play.
    void shuttleForward();
    /// J: reverse shuttle (see shuttleForward()).
    void shuttleReverse();
    /// K: pause.
    void shuttleStop();
    /// Physical J/K/L key state (from a key event filter) for K+L / K+J.
    void setShuttleKeyHeld(ShuttleControl::Key key, bool held);
    /// Pauses and moves by @p frames (clamped to the timeline).
    void step(int64_t frames);
    /// Moves by one second of frames (@p direction +1 / -1).
    void stepSeconds(int direction);
    /// Moves to @p frame (keeps playing if playing; see "Seeking while playing").
    void seek(int64_t frame);
    /// Turns looping on/off.
    void setLoop(bool loop);
    /// Plays from the in mark to the out mark (whole timeline if unmarked).
    void playInToOut();
    /// Plays from pre-roll before to post-roll after the current frame once, then returns to
    /// it (PlaybackBehaviour; Resolve's "play around current frame").
    void playAroundCurrent();
    /// Moves to the in mark / to the last frame before the out mark.
    void goToIn();
    void goToOut();
    /// Moves to the first / last frame.
    void goToStart();
    void goToEnd();

Q_SIGNALS:
    /// A new frame is to be shown (also re-emitted for stills after scrubbing or edits).
    void frameReady(const RenderedFramePtr& frame);
    /// The position changed (every frame during playback; unthrottled).
    void positionChanged(int64_t frame);
    void stateChanged(PlaybackEngine::State state);
    void rateChanged(double rate);
    void loopChanged(bool loop);
    /// The quality used for rendering changed (Auto mode switching, or the setting).
    void qualityChanged(Quality quality);
    /// proxiesInUse() changed (viewer "Proxy" indicator).
    void proxiesInUseChanged(bool inUse);

private:
    void init(Options options);
    void onTick();
    void onTimelineChanged();
    void onPlayheadChanged(int64_t frame);
    void startMotion(double rate, double fromFrame);
    void seekWhilePlaying(int64_t frame, bool writePlayhead);
    void resumeFromPark();
    void stopMotion();
    void startAudioClock(double fromFrame);
    void stopAudio();
    void applyShuttle(const ShuttleControl::Command& cmd);
    void updateParams(bool rerender);
    void requestStill(bool force);
    void present(const RenderedFramePtr& frame, int64_t key);
    void setPositionInternal(int64_t frame, bool writePlayhead);
    void writePlayhead();
    void ensureTicking();
    void updateAutoQuality(int64_t nowUs);
    void logStats(int64_t nowUs);
    void logStatsSummary();
    void setRateInternal(double rate);
    [[nodiscard]] double wrap(double unwrapped) const;
    [[nodiscard]] int64_t clampFrame(int64_t frame) const;
    [[nodiscard]] Rational fps() const;
    /// [start, end) of the active loop / play range in frames.
    void range(int64_t* start, int64_t* end) const;
    [[nodiscard]] bool loopActive() const;

    std::shared_ptr<Compositor> m_compositor;
    std::unique_ptr<AudioOutput> m_audio;
    std::unique_ptr<AudioRing> m_ring;
    std::unique_ptr<AudioFeeder> m_feeder;
    FrameScheduler m_scheduler;
    PlaybackClock m_clock;
    ShuttleControl m_shuttle;
    QTimer m_tick;
    QTimer m_shuttleTimer; ///< fires at ShuttleControl::holdDeadlineMs()

    Timeline* m_timeline = nullptr;
    MediaPool* m_pool = nullptr;
    std::shared_ptr<const TimelineData> m_snapshot;
    std::shared_ptr<const MediaPathMap> m_media;
    std::shared_ptr<const MediaPathMap> m_audioOverride; ///< setSubstitutes()
    std::shared_ptr<const ProxyMap> m_proxies;
    std::shared_ptr<const RenderCacheView> m_renderCache;
    bool m_proxiesInUse = false;
    QMetaObject::Connection m_timelineChanged, m_playheadChanged, m_poolChanged;

    double m_rate = 0.0;
    int64_t m_position = 0;
    bool m_loop = false;
    bool m_customRange = false; ///< playing m_customStart..m_customEnd once (in-out / around)
    int64_t m_customStart = 0;
    int64_t m_customEnd = 0;
    int64_t m_returnTo = -1; ///< frame to go back to when the custom range finishes
    PlaybackBehaviour m_behaviour;
    int64_t m_rangeStart = 0; ///< range captured when motion started (loop / in-out)
    int64_t m_rangeEnd = 0;
    bool m_rangeLoop = false;
    bool m_muted = false;
    float m_volume = 1.0f;
    bool m_settingPlayhead = false; ///< guards our own Timeline::setPlayhead calls
    int64_t m_lastPlayheadWriteUs = 0;
    int64_t m_audioStartUs = 0;
    int64_t m_motionStartUs = 0;

    bool m_parked = false; ///< playing, but held at a seek target (see seekWhilePlaying())
    bool m_resuming = false; ///< startMotion() called by resumeFromPark()
    bool m_parkShown = false; ///< the seek target is on screen
    int64_t m_parkTarget = 0;
    int64_t m_lastSeekUs = 0;

    RenderedFramePtr m_current;
    int64_t m_lastShownKey = 0;
    bool m_hasShownKey = false;

    struct Client
    {
        QSize size;
        bool textures = false;
    };
    QHash<const void*, Client> m_clients;
    QualityMode m_qualityMode = QualityMode::Full;
    AutoQuality m_auto;
    Quality m_lastEmittedQuality = Quality::Full;
    bool m_logStats = false; ///< KK_PLAYBACK_STATS=1: one line per second + one per play
    int64_t m_statsLogUs = 0;
    Stats m_statsAtLog;
    bool m_playStatsActive = false; ///< a play (from rest) is being summarised
    Stats m_statsAtPlay; ///< stats() when the current play started
    double m_playStageSumAtStart[4] = {0, 0, 0, 0};
    int64_t m_playStageCountAtStart = 0;
    double m_statsPlayRate = 1.0; ///< rate of the play being summarised (last one if changed)

    Stats m_stats;
    double m_stageSum[4] = {0, 0, 0, 0}; ///< decode, upload, composite, output (ms)
    int64_t m_stageCount = 0;
    double m_winSum[4] = {0, 0, 0, 0}; ///< the same since the last stats log line
    int64_t m_winCount = 0;
};

} // namespace kk
