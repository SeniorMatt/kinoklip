// SPDX-License-Identifier: GPL-3.0-or-later
// TransformOverlay — on-screen Transform controls of the timeline viewer: the outline of the
// current clip's picture, dragged to move the clip (titles, picture in picture).
#pragma once

#include <QPointF>
#include <QTransform>
#include <QWidget>

#include <optional>

class QAction;

namespace kk {

struct AppContext;
struct ClipData;
class ViewerSurface;

/// Transparent child of a timeline viewer's picture surface, shown while the on-screen
/// controls are set to Transform ("view.onscreen_transform") and the current clip (the last
/// clicked, SelectionModel::currentClip()) is a video clip under the playhead. Like
/// MaskOverlay it follows the viewer's Fit / zoom mapping and the clip's own transform.
///
/// Draws the clip's frame outline and centre; dragging anywhere in the viewer moves the clip
/// (Transform Position X / Y, in timeline pixels; Shift keeps one axis). One undo step per
/// drag ("Move Clip"); an animated position gets a key at the playhead, as an Inspector edit.
/// Esc during the drag cancels it.
///
/// Thread-safety: GUI thread only.
class TransformOverlay : public QWidget
{
    Q_OBJECT
public:
    /// Creates the overlay over @p surface (its widget becomes the parent).
    TransformOverlay(AppContext& ctx, ViewerSurface* surface);
    ~TransformOverlay() override;

    /// True if the controls are shown and take the mouse.
    [[nodiscard]] bool isEditing() const { return m_editing; }
    /// Timeline frame coordinates (px, origin at the top left) → overlay widget coordinates,
    /// or std::nullopt when nothing is shown. Tests use it to aim the mouse.
    [[nodiscard]] std::optional<QTransform> frameToWidget() const;
    /// Re-evaluates visibility and repaints (zoom, timeline or selection changes).
    void refresh();

Q_SIGNALS:
    /// The user pressed on the overlay (the viewer activates itself).
    void pressed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// The current clip if it is shown and editable, else nullptr.
    [[nodiscard]] const ClipData* currentClip(quint64* id = nullptr) const;
    void finishDrag(bool commit);

    AppContext& m_ctx;
    ViewerSurface* m_surface;
    QAction* m_action = nullptr;
    bool m_editing = false;
    bool m_dragging = false;
    quint64 m_dragClip = 0;
    QPointF m_pressPos; ///< widget px
    QPointF m_startPos; ///< Position X / Y at the press (timeline px)
};

} // namespace kk
