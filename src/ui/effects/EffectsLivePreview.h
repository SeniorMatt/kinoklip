// SPDX-License-Identifier: GPL-3.0-or-later
// EffectsLivePreview — Resolve's "Live Preview": while the pointer rests on an entry of the
// Effects library, the timeline viewer shows the frame as it would look with that entry
// applied, rendered from a private copy of the timeline (the project is never touched).
#pragma once

#include "core/TimelineData.h"
#include "render/RenderTypes.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>

class QTimer;

namespace kk {

struct AppContext;
class ViewerPanel;

namespace livepreview {

/// What a hovered entry shows: a private copy of the timeline with the entry applied, and the
/// frame of that copy to render.
struct Scene
{
    std::shared_ptr<const TimelineData> timeline;
    int64_t frame = 0;
};

/// The clip an effect or transition preview acts on: the topmost selected video clip under
/// the playhead, else the topmost video clip there (enabled clips on enabled tracks only).
/// 0 = none.
[[nodiscard]] quint64 targetClip(
    const TimelineData& data, int64_t playhead, const QVector<quint64>& selected);

/// The scene for descriptor @p descriptorId over @p snapshot (Timeline::snapshot()):
/// - video effect: added with its default parameters to targetClip(), at the playhead;
/// - video transition: put at the end of targetClip() nearer to the playhead (an existing
///   transition there keeps its length and alignment, else one second, centred on a cut);
///   the frame is the transition's start + @p position of its length (0.5 = the midpoint);
/// - title / generator: a clip of the default generator length on a new top track, placed
///   so that the playhead sits at @p position of it (titles animate: scrubbing), the frame is
///   the playhead.
/// @p position is the pointer's horizontal place over the entry, 0..1 (clamped).
/// std::nullopt when the entry has nothing to show: audio effects and transitions, the
/// Adjustment Clip (it changes nothing with its defaults), no clip under the playhead for an
/// effect or transition, an unknown id.
[[nodiscard]] std::optional<Scene> buildScene(const TimelineData& snapshot, int64_t playhead,
    const QVector<quint64>& selected, const QString& descriptorId, double position);

/// A scene prepared once for an entry over one timeline state (snapshot, playhead,
/// selection); scrubbing then only moves the time, without rebuilding the timeline copy:
/// - effect: one picture whatever the position;
/// - transition: the copy stays, only the frame changes;
/// - title / generator: the copy stays, only the clip's start changes, in place when no
///   render holds the copy any more (else on a fresh copy, counted in copyCount()).
///
/// Thread-safety: GUI thread; the scenes handed out are immutable while a render holds them.
class PreparedScene
{
public:
    /// std::nullopt when the entry has nothing to show (see buildScene()).
    [[nodiscard]] static std::optional<PreparedScene> prepare(const TimelineData& snapshot,
        int64_t playhead, const QVector<quint64>& selected, const QString& descriptorId);

    /// Whether the position moves the picture (transitions, titles, generators).
    [[nodiscard]] bool scrubs() const { return m_kind != Kind::Fixed; }
    /// Frames the position runs over (1 for an effect).
    [[nodiscard]] int64_t length() const { return m_length; }
    /// The frame step (0 .. length() - 1) of @p position (0..1, clamped). Pass the step
    /// currently shown as @p current to ignore jitter: a step is only left once the pointer
    /// is kJitterFrames past the half-way mark to the next one.
    [[nodiscard]] int64_t stepAt(double position, int64_t current = -1) const;
    /// The scene at step @p step (clamped).
    [[nodiscard]] Scene sceneAt(int64_t step);
    /// Timeline copies made after prepare() (title scenes still held by a render).
    [[nodiscard]] int copyCount() const { return m_copies; }

    static constexpr double kJitterFrames = 0.25; ///< hysteresis around a step boundary

private:
    enum class Kind {
        Fixed,
        Transition,
        Clip
    };
    Kind m_kind = Kind::Fixed;
    std::shared_ptr<TimelineData> m_timeline;
    int64_t m_frame = 0; ///< Fixed: the frame; Transition: window start; Clip: the playhead
    int64_t m_length = 1;
    size_t m_track = 0; ///< Clip: video track of the generator clip (alone on it)
    int m_copies = 0;
};

} // namespace livepreview

/// Drives the live preview of one timeline viewer: hover() asks for an entry, stop() ends it.
///
/// - A new entry is rendered after kDebounceMs without a further change (the pointer sweeping
///   over the list renders nothing); moving along the same entry (scrubbing) renders at once,
///   one render in flight: moves during a render are coalesced, only the newest position is
///   rendered when it is done (latest wins). A move that maps to the frame already shown
///   renders nothing.
/// - The scene is prepared once per entry and timeline state (livepreview::PreparedScene);
///   scrubbing only moves its time.
/// - Renders run on the compositor's render thread (PlaybackEngine::renderAlternative());
///   the GUI thread polls the future. A request made stale by another entry or stop() is
///   cancelled (skipped if still queued) and its result is never shown.
/// - The picture is drawn with ViewerPanel::showTemporaryFrame(); stop() restores the
///   engine's picture. Playback, any timeline edit, a timeline switch or a project reset
///   stop the preview; a playhead move or a selection change re-renders it.
/// - The project, its undo stack and the playhead are never changed.
///
/// Thread-safety: GUI thread only.
class EffectsLivePreview : public QObject
{
    Q_OBJECT
public:
    static constexpr int kDebounceMs = 100; ///< quiet time before a new entry is rendered
    static constexpr int kPollMs = 4; ///< render-result poll interval

    /// Shows its previews in @p viewer (a Timeline mode viewer; not owned).
    EffectsLivePreview(AppContext& ctx, ViewerPanel* viewer, QObject* parent = nullptr);
    ~EffectsLivePreview() override;

    /// The pointer rests on descriptor @p descriptorId at horizontal @p position (0..1).
    void hover(const QString& descriptorId, double position);
    /// Ends the preview: pending and running requests are dropped, the viewer shows the
    /// normal picture again.
    void stop();

    /// Entry being previewed or waited for (empty = none).
    [[nodiscard]] QString hoveredId() const { return m_wantedId; }
    /// Entry whose picture the viewer shows now (empty = the normal picture).
    [[nodiscard]] QString shownId() const { return m_shownId; }
    /// Renders requested / cancelled as stale since construction (tests).
    [[nodiscard]] int requestCount() const { return m_requests; }
    [[nodiscard]] int cancelledCount() const { return m_cancelled; }
    /// Issues the debounced request now (tests).
    void flushDebounce();
    /// Scenes prepared since construction: one per entry and timeline state, not one per
    /// scrub position (tests).
    [[nodiscard]] int sceneBuildCount() const { return m_sceneBuilds; }
    /// True while a render is in flight (tests).
    [[nodiscard]] bool rendering() const { return m_running.has_value(); }
    /// Frame step of the entry the viewer shows (-1 = none; tests).
    [[nodiscard]] int64_t shownStep() const { return m_shownStep; }
    /// The prepared scene of the hovered entry, if any (tests).
    [[nodiscard]] const livepreview::PreparedScene* preparedScene() const
    {
        return m_prepared ? &*m_prepared : nullptr;
    }

Q_SIGNALS:
    /// The viewer now shows the preview of @p descriptorId.
    void previewShown(const QString& descriptorId);

private:
    /// Renders the wanted entry (or marks it pending while a render runs).
    void request();
    /// Picks up a finished render.
    void collect();
    /// Drops the running render, if any (it is skipped if still queued).
    void cancelRunning();
    /// Re-renders after a playhead or selection change, if a preview is active.
    void refresh();
    /// Prepares the wanted entry's scene unless the one kept is for the same entry and
    /// timeline state. False: nothing to show.
    bool ensurePrepared();
    /// Forgets the prepared scene and the shown step.
    void dropPrepared();

    AppContext& m_ctx;
    QPointer<ViewerPanel> m_viewer;
    QTimer* m_debounce = nullptr;
    QTimer* m_poll = nullptr;
    QString m_wantedId;
    double m_wantedPosition = 0.5;
    QString m_shownId;

    /// The render in flight.
    struct Running
    {
        std::future<RenderedFramePtr> future;
        std::shared_ptr<std::atomic<bool>> cancel;
        QString id;
        int64_t step = 0;
        quint64 generation = 0;
    };
    std::optional<Running> m_running;
    bool m_pending = false; ///< the wish changed while a render ran (latest wins)

    /// The scene kept for scrubbing and the state it was made for.
    struct PreparedKey
    {
        QString id;
        std::shared_ptr<const TimelineData> snapshot;
        int64_t playhead = 0;
        QVector<quint64> selected;
    };
    std::optional<PreparedKey> m_preparedKey;
    std::optional<livepreview::PreparedScene> m_prepared; ///< empty with a key: nothing to show
    quint64 m_generation = 0; ///< increases with every prepared scene
    quint64 m_shownGeneration = 0;
    int64_t m_shownStep = -1;
    int m_sceneBuilds = 0;
    int m_requests = 0;
    int m_cancelled = 0;
};

} // namespace kk
