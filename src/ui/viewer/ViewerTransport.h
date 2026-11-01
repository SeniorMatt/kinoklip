// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerTransport — connects the playback-owned global actions (transport.*,
// view.fullscreen_viewer) to the active viewer's engine; J/K/L hold detection; full-screen viewer.
#pragma once

#include <QObject>
#include <QPointer>

class QKeyEvent;

namespace kk {

struct AppContext;
class PlaybackEngine;
class ViewerPanel;

/// Application-wide transport routing.
///
/// * **Target**: the active ViewerPanel (the last one that received focus or a click, see
///   ViewerPanel::activate()) if it is visible; otherwise the first visible Timeline viewer,
///   otherwise the first visible viewer; with no visible viewer the program engine
///   (AppContext::playback). Focus entering a widget tagged with the dynamic property
///   `kkProgramFocus` (the timeline panel) activates the visible Timeline viewer, so the
///   viewer layout is never assumed: pages only tag widgets.
/// * **Actions**: transport.play_pause, shuttle_reverse/stop/forward (J/K/L), prev/next_frame,
///   back/forward_1s, play_in_out, play_around and view.fullscreen_viewer. NOT the navigation
///   actions owned by the timeline (prev/next_edit, go_to_start/end) nor transport.loop
///   (each viewer applies that global toggle to its engine).
/// * **J/K/L held**: an application event filter watches the physical keys bound to the
///   shuttle actions (looked up in the ActionRegistry, so remapping works) and tells the
///   target engine which are held (K+L / K+J step and slow play).
/// * **Full-screen viewer**: a borderless Timeline viewer on the main window's screen,
///   picture only; all registry actions are added to it so shortcuts keep working; Esc or
///   the action again closes it.
///
/// Thread-safety: GUI thread only.
class ViewerTransport : public QObject
{
    Q_OBJECT
public:
    /// @p ctx must outlive this object.
    explicit ViewerTransport(AppContext& ctx, QObject* parent = nullptr);
    ~ViewerTransport() override;

    /// Connects the playback-owned actions (call once; no-op without a registry).
    void connectActions();
    /// The viewer the actions act on (see class doc), or nullptr.
    [[nodiscard]] ViewerPanel* targetViewer() const;
    /// The engine the actions act on, or nullptr.
    [[nodiscard]] PlaybackEngine* targetEngine() const;
    /// Opens / closes the full-screen program viewer.
    void setFullScreenViewer(bool on);
    [[nodiscard]] bool isFullScreenViewerOpen() const { return !m_fullScreen.isNull(); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onFocusChanged(QWidget* old, QWidget* now);
    /// 0 = J, 1 = K, 2 = L, -1 = none for a key event.
    [[nodiscard]] int shuttleKey(const QKeyEvent* key) const;

    AppContext& m_ctx;
    bool m_connected = false;
    QPointer<PlaybackEngine> m_heldEngine[3];
    QPointer<ViewerPanel> m_fullScreen;
};

} // namespace kk
