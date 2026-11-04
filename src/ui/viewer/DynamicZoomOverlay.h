// SPDX-License-Identifier: GPL-3.0-or-later
// DynamicZoomOverlay — on-screen Dynamic Zoom controls of the timeline viewer: the current
// clip's start (green) and end (red) framing rectangles over its whole picture.
#pragma once

#include "core/DynamicZoom.h"

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
/// controls are set to Dynamic Zoom ("view.onscreen_dynamic_zoom") and the current clip is a
/// video clip under the playhead (onScreenControlsClip()). Meanwhile the viewer renders the
/// clips without their Dynamic Zoom (PlaybackEngine::setDynamicZoomApplied(false)), so the
/// rectangles are drawn over the whole picture, through the clip's own transform.
///
/// Dragging inside a rectangle moves it; dragging a corner resizes it about the opposite
/// corner, keeping the frame's aspect. The end rectangle wins where they overlap (it is drawn
/// on top). One undo step per drag ("Move Dynamic Zoom" / "Resize Dynamic Zoom"); Esc during
/// the drag cancels it (spec 24.20).
///
/// Thread-safety: GUI thread only.
class DynamicZoomOverlay : public QWidget
{
    Q_OBJECT
public:
    /// Which rectangle and which part of it the pointer grabs.
    struct Handle
    {
        enum class Part {
            None,
            Move, ///< inside the rectangle
            Corner ///< a corner (sx, sy = -1 / +1)
        };
        Part part = Part::None;
        bool end = false; ///< the end (red) rectangle, else the start (green) one
        int sx = 0;
        int sy = 0;
    };

    DynamicZoomOverlay(AppContext& ctx, ViewerSurface* surface);
    ~DynamicZoomOverlay() override;

    /// True if the controls are shown and take the mouse.
    [[nodiscard]] bool isEditing() const { return m_editing; }
    /// Picture coordinates (clip content, frame px around the frame centre) -> overlay widget
    /// coordinates of the shown clip, or std::nullopt (tests aim with it).
    [[nodiscard]] std::optional<QTransform> pictureToWidget() const;
    /// The handle under @p pos (widget coordinates).
    [[nodiscard]] Handle hitTest(QPointF pos) const;
    /// Re-evaluates visibility, the viewer's Dynamic Zoom and repaints.
    void refresh();

    /// Distance (widget px) within which a corner is grabbed.
    static constexpr double kGrabRadius = 7.0;

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
    [[nodiscard]] const ClipData* currentClip(quint64* id = nullptr) const;
    [[nodiscard]] bool isOn() const;
    void finishDrag(bool commit);

    AppContext& m_ctx;
    ViewerSurface* m_surface;
    QAction* m_action = nullptr; ///< view.onscreen_dynamic_zoom
    bool m_editing = false;
    bool m_dragging = false;
    quint64 m_dragClip = 0;
    Handle m_handle;
    QPointF m_pressPicture; ///< pointer at the press, picture coordinates
    dynamiczoom::Rect m_pressRect; ///< the grabbed rectangle at the press
};

} // namespace kk
