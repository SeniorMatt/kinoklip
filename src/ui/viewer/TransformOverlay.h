// SPDX-License-Identifier: GPL-3.0-or-later
// TransformOverlay — on-screen Transform and Crop controls of the timeline viewer: the
// current clip's picture outline with move, scale, rotate, anchor and crop handles.
#pragma once

#include "ui/viewer/TransformGesture.h"

#include <QPointF>
#include <QPointer>
#include <QTransform>
#include <QWidget>

#include <optional>

class QAction;

namespace kk {

struct AppContext;
struct ClipData;
class GuideOverlay;
class ViewerSurface;

/// The clip the viewer's on-screen controls act on: the current clip (the last clicked,
/// SelectionModel::currentClip(); the video part of a linked pair) when it is a video clip on
/// an unlocked track under the playhead, else nullptr.
[[nodiscard]] const ClipData* onScreenControlsClip(const AppContext& ctx, quint64* id = nullptr);

/// Transparent child of a timeline viewer's picture surface, shown while the on-screen
/// controls are set to Transform ("view.onscreen_transform") or Crop ("view.onscreen_crop")
/// and the current clip (the last clicked, SelectionModel::currentClip()) is a video clip
/// under the playhead. Like MaskOverlay it follows the viewer's Fit / zoom mapping and the
/// clip's own transform.
///
/// Transform (spec 24.9, 24.11): the picture outline with corner and edge handles (Zoom),
/// a rotation knob above the top edge (Rotation), the anchor point cross (Anchor X / Y) and
/// dragging anywhere else moves the clip (Position X / Y). Crop (spec 24.12): the uncropped
/// outline dimmed and the crop rectangle with edge and corner handles (Crop Left / Right /
/// Top / Bottom). The maths and the Shift conventions are in transformgesture::TransformDrag.
///
/// One named undo step per drag ("Move Clip", "Scale Clip", "Rotate Clip", "Move Anchor
/// Point", "Crop Clip"); an animated parameter gets a key at the playhead, as an Inspector
/// edit. Esc during the drag cancels it.
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
    /// True while the Crop controls are shown (false: Transform).
    [[nodiscard]] bool isCropMode() const;
    /// True when a handle (not the plain move area) is under @p pos, in this widget's coordinates.
    [[nodiscard]] bool handleAt(QPointF pos) const;
    /// Current picture geometry of the shown clip, or std::nullopt (tests aim with it).
    [[nodiscard]] std::optional<TransformPicture> picture() const;
    /// Timeline frame coordinates (px, origin at the top left) → overlay widget coordinates,
    /// or std::nullopt when nothing is shown. Tests use it to aim the mouse.
    [[nodiscard]] std::optional<QTransform> frameToWidget() const;
    /// Re-evaluates visibility and repaints (zoom, timeline or selection changes).
    void refresh();
    /// Guides to snap to while dragging (spec 41.9): Move snaps the picture's edges and
    /// centre, Anchor the anchor point, Scale and Crop the dragged edges onto guides within
    /// kSnapPx when GuideOverlay::snapTargets() offers any; Ctrl held skips snapping.
    void setGuideOverlay(GuideOverlay* guides);
    /// Snap distance (widget px).
    static constexpr double kSnapPx = 6.0;

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
    /// Pointer cursor for the handle under @p pos.
    void updateCursor(QPointF pos);
    /// The pointer (frame px) moved so the dragged feature lands on a guide in reach, or
    /// @p pointer unchanged. Re-runs the drag maths on @p clip at @p localFrame.
    [[nodiscard]] QPointF snapPointer(ClipData& clip, int64_t localFrame, QPointF pointer,
        bool shift, const QTransform& toWidget);

    AppContext& m_ctx;
    ViewerSurface* m_surface;
    QAction* m_action = nullptr; ///< view.onscreen_transform
    QAction* m_cropAction = nullptr; ///< view.onscreen_crop
    bool m_editing = false;
    bool m_dragging = false;
    quint64 m_dragClip = 0;
    transformgesture::TransformDrag m_drag;
    QPointer<GuideOverlay> m_guides; ///< setGuideOverlay()
};

} // namespace kk
