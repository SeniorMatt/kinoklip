// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewer — one of the Compositing page's two viewers: shows the output of one node of the
// open composition, with the node name in the header, zoom, guides and on-viewer controls.
#pragma once

#include "render/RenderTypes.h"
#include "ui/common/PanelFrame.h"
#include "ui/comp/ViewerFx.h"

#include <QFutureWatcher>
#include <QPointer>

class QAction;
class QComboBox;
class QKeyEvent;
class QLabel;
class QToolBar;

namespace kk {

struct AppContext;
class CompSession;
class CompViewerOverlay;
class GuideOverlay;
class PlaybackEngine;
class SplitHandle;
class ViewerSurface;

/// A viewer of the Compositing page (index 0 = left, 1 = right).
///
/// The header shows "Left" / "Right" with the shown node's name (centred), the zoom (Fit, 50,
/// 100, 200 %) on the left and the comp frame on the right. A node arrives by the 1 / 2 keys
/// in the node editor (CompSession::setViewerNode()) or by dragging a node onto the viewer
/// (mime::CompNode). The picture surface is the Edit page viewer's (GL when available), with
/// the same guide / safe area layer, and a CompViewerOverlay with the on-viewer controls of
/// the selected node.
///
/// Picture: the node's own image at the comp frame (PlaybackEngine::renderCompositionNode(),
/// with the engine's media table, proxies and quality; zero copy, outside the render cache),
/// re-rendered on graph, playhead and viewer changes with at most one render in flight. A clip
/// without a stored composition shows the program picture (the default graph renders exactly the
/// clip).
///
/// Viewer tools (a tool bar under the header that folds into its » menu, and keys that act only
/// while this viewer has the focus; nothing is a global shortcut): buffers A and B (`,` `.`
/// and `/` for a split wipe with a draggable line), the channel shown (C colour, R, G, B, A),
/// a checker under the picture, Show Controls (Ctrl+K), Lock Viewer (Ctrl+L), Clear Viewers
/// (`~`) and 100 % zoom (Ctrl+1). Channel, checker and split work on the CPU image of the
/// render (so they need a stored composition); plain viewing stays on the GL texture.
///
/// Thread-safety: GUI thread only.
class CompViewer : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates viewer @p index (0 left, 1 right) of @p session.
    CompViewer(int index, AppContext& ctx, CompSession* session, QWidget* parent = nullptr);
    ~CompViewer() override;

    /// 0 = left, 1 = right.
    [[nodiscard]] int index() const { return m_index; }
    /// The node shown (0 = none).
    [[nodiscard]] quint64 node() const;
    /// The text of the header (node name or "No node"), for tests.
    [[nodiscard]] QString headerText() const;
    /// The picture surface.
    [[nodiscard]] ViewerSurface* surface() const { return m_surface; }
    /// The guides / safe areas layer.
    [[nodiscard]] GuideOverlay* guideOverlay() const { return m_guides; }
    /// The on-viewer controls of the selected node.
    [[nodiscard]] CompViewerOverlay* controls() const { return m_controls; }
    /// Picture zoom: 0 = fit, else device pixels per timeline pixel.
    void setZoom(double zoom);

    // ---- Viewer tools ----
    /// The buffer shown (0 = A, 1 = B). A is the node the session assigns (1 / 2 keys); B holds
    /// the node assigned while B is shown (copied from A the first time).
    [[nodiscard]] int activeBuffer() const { return m_active; }
    void setActiveBuffer(int buffer);
    /// Node of buffer @p buffer (0 = empty).
    [[nodiscard]] quint64 bufferNode(int buffer) const;
    /// Puts @p nodeId into buffer @p buffer.
    void setBufferNode(int buffer, quint64 nodeId);
    /// Split display: A on one side of a line, B on the other.
    [[nodiscard]] bool splitShown() const { return m_split; }
    void setSplit(bool on);
    [[nodiscard]] QPointF splitPos() const { return m_splitPos; }
    [[nodiscard]] double splitAngle() const { return m_splitAngle; }
    void setSplitLine(QPointF pos, double angleDeg);
    [[nodiscard]] viewerfx::Channel channel() const { return m_channel; }
    void setChannel(viewerfx::Channel channel);
    [[nodiscard]] bool checkerShown() const { return m_checker; }
    void setChecker(bool on);
    [[nodiscard]] bool controlsShown() const { return m_controlsShown; }
    void setControlsShown(bool on);
    [[nodiscard]] bool locked() const { return m_locked != 0; }
    /// Locks the viewer on the node it shows now (it ignores 1 / 2 and node drops), or unlocks.
    void setLocked(bool on);
    /// Empties the viewer (both buffers, lock off, no node).
    void clearViewer();
    /// 100 % zoom, picture centred (Ctrl+1).
    void zoomTo100();
    /// The tool bar (actions named "CompViewerTool:<id>").
    [[nodiscard]] QToolBar* toolBar() const { return m_bar; }
    /// The tool bar action @p id (a, b, split, color, red, green, blue, alpha, checker,
    /// controls, lock, clear, zoom100).
    [[nodiscard]] QAction* toolAction(const QString& id) const;

signals:
    /// Clear Viewers was used: the page empties both viewers.
    void clearRequested();

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void refresh();
    void updateHeader();
    void updateOutputClient();
    void showFrame(const RenderedFramePtr& frame);
    /// True when the viewer renders its node itself (a stored composition and a compositor).
    [[nodiscard]] bool rendersNodes() const;
    /// Queues PlaybackEngine::renderCompositionNode() for the node at the comp frame.
    void requestRender();
    [[nodiscard]] bool followsEngine() const;
    void updateEngineView();
    void onRendered();
    /// The viewer tool a key press means (empty: not ours).
    [[nodiscard]] QString keyTool(const QKeyEvent* event) const;
    void runTool(const QString& id);
    void buildToolBar();
    void syncToolBar();
    void onSessionViewers();
    /// True when the picture needs the CPU image path (channel, checker, split).
    [[nodiscard]] bool needsFx() const;
    [[nodiscard]] quint64 shownNode() const;

    AppContext& m_ctx;
    QPointer<CompSession> m_session;
    int m_index;
    QPointer<PlaybackEngine> m_engine;
    ViewerSurface* m_surface = nullptr;
    GuideOverlay* m_guides = nullptr;
    CompViewerOverlay* m_controls = nullptr;
    QComboBox* m_zoom = nullptr;
    QLabel* m_frameLabel = nullptr;
    QFutureWatcher<RenderedFramePtr>* m_watcher = nullptr;
    QToolBar* m_bar = nullptr;
    SplitHandle* m_handle = nullptr;
    int m_active = 0;
    quint64 m_nodeA = 0; ///< buffer A: the session's node
    quint64 m_nodeB = 0;
    quint64 m_locked = 0;
    bool m_split = false;
    QPointF m_splitPos {0.5, 0.5};
    double m_splitAngle = 90.0;
    viewerfx::Channel m_channel = viewerfx::Channel::Color;
    bool m_checker = false;
    bool m_controlsShown = true;
    quint64 m_seen = 0; ///< the session's node for this viewer at the last change
    bool m_empty = false; ///< Clear Viewers: shows nothing until a node is assigned
    bool m_capturing = false; ///< guard while B takes the session's node
    bool m_inFlight = false;
    bool m_renderAgain = false;
};

} // namespace kk
