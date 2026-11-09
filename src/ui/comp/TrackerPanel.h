// SPDX-License-Identifier: GPL-3.0-or-later
// TrackerPanel — the Tracker node's controls in the Compositing Inspector: track backward,
// stop, track forward, a progress bar and a status line; runs the tracking and stores the
// result in the node.
#pragma once

#include "compositing/tracking/TrackerRunner.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QProgressBar;
class QToolButton;

namespace kk {

struct AppContext;
class CompSession;

/// The panel is created once by the CompInspector and moved into the parameter area whenever a
/// Tracker node is shown, so a track keeps running while another node is selected.
///
/// **Flow.** Track Forward / Backward starts a TrackerRunner at the playhead: the point is
/// where the node's X / Y are at that frame; the pictures are the node's tracking source (or its
/// input) rendered by the playback engine (PlaybackEngine::renderCompositionNode(), the same
/// path as the viewers). The playhead follows the tracking. When the run ends (done, stopped,
/// lost) the positions found are stored in the node as keyframes in ONE undo step
/// (tracking::storeTrack()). Opening another clip stops the run and drops the result.
///
/// Thread-safety: GUI thread only.
class TrackerPanel : public QWidget
{
    Q_OBJECT
public:
    TrackerPanel(AppContext& ctx, CompSession* session, QWidget* parent = nullptr);

    /// The Tracker node shown (0 = none).
    void setNode(quint64 nodeId);
    /// Starts tracking the shown node from the playhead; false if it cannot start (no picture
    /// to track, no engine, nothing to track in that direction, locked node). The reason is in
    /// statusText().
    bool track(bool forward);
    void stop();
    [[nodiscard]] bool isRunning() const { return m_runner.isRunning(); }
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] tracking::TrackerRunner* runner() { return &m_runner; }

    // Widgets (tests).
    [[nodiscard]] QToolButton* backwardButton() const { return m_backward; }
    [[nodiscard]] QToolButton* stopButton() const { return m_stop; }
    [[nodiscard]] QToolButton* forwardButton() const { return m_forward; }
    [[nodiscard]] QProgressBar* progressBar() const { return m_bar; }

private:
    void updateState();
    void onFinished(tracking::TrackOutcome outcome, const QString& reason);
    void setStatus(const QString& text);

    AppContext& m_ctx;
    QPointer<CompSession> m_session;
    tracking::TrackerRunner m_runner;
    quint64 m_node = 0; ///< node shown
    quint64 m_runNode = 0; ///< node being tracked
    int64_t m_runStart = 0;
    bool m_discard = false;
    QToolButton* m_backward = nullptr;
    QToolButton* m_stop = nullptr;
    QToolButton* m_forward = nullptr;
    QToolButton* m_clear = nullptr;
    QProgressBar* m_bar = nullptr;
    QLabel* m_status = nullptr;
};

} // namespace kk
