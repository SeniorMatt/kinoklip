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

} // namespace livepreview

/// Drives the live preview of one timeline viewer: hover() asks for an entry, stop() ends it.
///
/// - A new entry is rendered after kDebounceMs without a further change (the pointer sweeping
///   over the list renders nothing); moving along the same entry (scrubbing) renders at once,
///   one render in flight, the newest wish following when it is done.
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
    };
    std::optional<Running> m_running;
    bool m_pending = false; ///< the wish changed while a render ran
    int m_requests = 0;
    int m_cancelled = 0;
};

} // namespace kk
