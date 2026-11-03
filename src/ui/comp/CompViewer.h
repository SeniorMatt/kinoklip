// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewer — one of the Compositing page's two viewers: shows the output of one node of the
// open composition, with the node name in the header, zoom, guides and on-viewer controls.
#pragma once

#include "render/RenderTypes.h"
#include "ui/common/PanelFrame.h"

#include <QPointer>

class QComboBox;
class QLabel;

namespace kk {

struct AppContext;
class CompSession;
class CompViewerOverlay;
class GuideOverlay;
class PlaybackEngine;
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
/// Picture: until the composition evaluation is merged, a viewer with a node shows the
/// program picture (the clip under the playhead, i.e. what MediaIn delivers); see
/// CompAdapter for the switch to the node render.
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

protected:
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

    AppContext& m_ctx;
    QPointer<CompSession> m_session;
    int m_index;
    QPointer<PlaybackEngine> m_engine;
    ViewerSurface* m_surface = nullptr;
    GuideOverlay* m_guides = nullptr;
    CompViewerOverlay* m_controls = nullptr;
    QComboBox* m_zoom = nullptr;
    QLabel* m_frameLabel = nullptr;
};

} // namespace kk
