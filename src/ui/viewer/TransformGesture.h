// SPDX-License-Identifier: GPL-3.0-or-later
// TransformGesture — maths of the viewer's on-screen Transform and Crop controls: picture
// geometry, handle hit-testing and what a drag writes into a clip's transform parameters.
#pragma once

#include "core/Animation.h"

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QTransform>

#include <cstdint>
#include <optional>
#include <vector>

namespace kk {

class MediaPool;
struct ClipData;

/// What the pointer grabs on the on-screen controls.
struct TransformHandle
{
    enum class Kind {
        None,
        Move, ///< inside or outside the picture: Position X / Y
        Anchor, ///< the anchor point cross: Anchor X / Y (the picture stays in place)
        Rotate, ///< the knob above the top edge: Rotation
        Scale, ///< a corner (sx, sy != 0) or edge handle: Zoom X / Y
        Crop ///< a crop edge or corner: Crop Left / Right / Top / Bottom
    };
    Kind kind = Kind::None;
    int sx = 0; ///< -1 = left, +1 = right, 0 = neither (Scale / Crop)
    int sy = 0; ///< -1 = top, +1 = bottom, 0 = neither (Scale / Crop)

    friend bool operator==(const TransformHandle&, const TransformHandle&) = default;
};

/// Picture geometry of a clip at one frame, for drawing and hit-testing the controls.
struct TransformPicture
{
    QSizeF content; ///< picture size at zoom 1 in timeline px (centred content coordinates)
    QSizeF sourcePx; ///< source pixels (the unit of the crop parameters)
    QRectF crop; ///< visible (cropped) part, content coordinates
    QPointF anchor; ///< anchor point, content coordinates
    QTransform contentToFrame; ///< render::clipTransform() of the clip, without Dynamic Zoom
};

namespace transformgesture {

/// Distance (widget px) within which a handle is grabbed.
inline constexpr double kGrabRadius = 7.0;
/// Distance (widget px) of the rotation knob from the top edge.
inline constexpr double kRotateKnobDistance = 28.0;

/// Source size of @p clip (media display size from @p pool, else @p frame) and the picture it
/// occupies at zoom 1 (scaled to fit), like the renderers.
[[nodiscard]] TransformPicture picture(
    const ClipData& clip, int64_t localFrame, QSizeF frame, const MediaPool* pool);

/// Handle positions in widget coordinates for drawing: corners and edge midpoints of the
/// picture (Transform) or of the crop rectangle (Crop), in the order TL, T, TR, R, BR, B,
/// BL, L, with the TransformHandle each one is.
[[nodiscard]] std::vector<std::pair<QPointF, TransformHandle>> handles(
    const TransformPicture& pic, const QTransform& frameToWidget, bool crop);
/// Widget position of the rotation knob (above the middle of the top edge).
[[nodiscard]] QPointF rotateKnob(const TransformPicture& pic, const QTransform& frameToWidget);

/// The handle under widget point @p pos: in Transform mode the anchor, the rotation knob,
/// corners, edges, else Move; in Crop mode the crop corners and edges, else None.
[[nodiscard]] TransformHandle hitTest(
    const TransformPicture& pic, const QTransform& frameToWidget, bool crop, QPointF pos);

/// One drag on the controls. begin() records the parameters at the press; update() writes
/// the parameters for the current pointer into a ParamSet (AnimatedValue::setValue(), so an
/// animated parameter gets a key at the frame, like an Inspector edit).
///
/// Decisions (spec 24.11): corners scale around the anchor point and keep the aspect while
/// Zoom X / Y are linked (the default), edges stretch one axis (both while linked); Shift
/// toggles the link for the drag (a free scale of a linked zoom unlinks it). Shift snaps a
/// rotation to 15 degree steps and keeps a move on one axis. Moving the anchor keeps the
/// picture still (Position compensates). Crop handles stop one source pixel before the
/// opposite edge.
class TransformDrag
{
public:
    /// Starts dragging @p handle of @p clip at clip-local frame @p localFrame; @p pointer is
    /// the press position in timeline frame px.
    void begin(const ClipData& clip, int64_t localFrame, const TransformPicture& pic,
        TransformHandle handle, QPointF pointer);
    /// Writes the parameters for pointer @p pointer (timeline frame px) at @p localFrame into
    /// @p transform (ClipData::transform). @p shift = Shift held.
    void update(ParamSet& transform, int64_t localFrame, QPointF pointer, bool shift);
    /// The dragged handle.
    [[nodiscard]] TransformHandle handle() const { return m_handle; }

private:
    TransformHandle m_handle;
    TransformPicture m_pic;
    QPointF m_press;
    QPointF m_pivot; ///< frame px of the anchor point at the press
    double m_posX = 0, m_posY = 0, m_zoomX = 1, m_zoomY = 1, m_rotation = 0;
    double m_anchorX = 0, m_anchorY = 0;
    double m_cropL = 0, m_cropR = 0, m_cropT = 0, m_cropB = 0;
    double m_flipX = 1, m_flipY = 1;
    bool m_linked = true;
    double m_lastAngle = 0; ///< pointer angle of the previous update (rotation unwrapping)
    double m_turned = 0; ///< accumulated rotation of this drag, degrees
};

/// The parameters the on-screen drags change, read at one frame (spec 24.9).
struct DragValues
{
    double posX = 0, posY = 0, zoomX = 1, zoomY = 1, rotation = 0;
    double cropL = 0, cropR = 0, cropT = 0, cropB = 0;
    [[nodiscard]] static DragValues read(const ParamSet& transform, int64_t localFrame);
};

/// Multi-clip drags (spec 24.1): the change the primary clip's drag made, from @p start to
/// @p now, applied to another selected clip that was at @p peerStart when the drag began, as
/// a relative change: Position, Rotation and Crop (source pixels) are moved by the same
/// amount, Zoom X / Y are multiplied by the same factor. Only parameters the primary changed
/// are written (AnimatedValue::setValue() at @p peerLocal, so an animated parameter gets a
/// key); @p peerSourcePx limits the crop of the peer to its own picture.
void applyRelative(const DragValues& start, const DragValues& now, const DragValues& peerStart,
    ParamSet& peerTransform, int64_t peerLocal, QSizeF peerSourcePx);

} // namespace transformgesture

} // namespace kk
