// SPDX-License-Identifier: GPL-3.0-or-later
// RetimeBar — geometry and hit-testing of the retime controls drawn on a clip (Ctrl+R): the
// speed bar with one field per speed segment, speed point lines with their two handles, and
// the retime handle at the clip end. Pure (no widgets), shared by painter and input.
#pragma once

#include "core/Retime.h"
#include "ui/timeline/TimelineLayout.h"

#include <QRectF>
#include <QVector>

#include <cstdint>

namespace kk::retimebar {

inline constexpr double kBarHeight = 16; ///< speed bar under the clip's name band (px)
inline constexpr double kNameBand = 15; ///< TimelinePainter's name band height (px)
inline constexpr double kHandleHalfWidth = 5; ///< speed point / end handle grab half width
inline constexpr double kBottomHandleHeight = 9; ///< retime handle at the bottom of a point
inline constexpr double kMinClipHeight = 34; ///< lower clips show no retime controls
inline constexpr double kArrowWidth = 12; ///< menu arrow area at the right of a segment

/// One segment field of the speed bar.
struct SegmentField
{
    int index = 0; ///< retime::segments() index
    QRectF rect; ///< field in view coordinates (clipped to the clip)
    QRectF arrow; ///< the menu arrow area (right end of the field)
    retime::Segment segment;
};

/// One speed point: its line and its two handles.
struct PointMarks
{
    int64_t localFrame = 0;
    double x = 0;
    QRectF top; ///< speed point handle (in the bar): moves where the speed changes
    QRectF bottom; ///< retime handle (bottom of the clip): changes the speed before it
};

/// The retime controls of one clip.
struct Geometry
{
    QRectF bar; ///< empty = clip too low / narrow
    QVector<SegmentField> fields;
    QVector<PointMarks> points;
    QRectF endHandle; ///< retime handle of the last segment at the clip end (right end of the bar)
    [[nodiscard]] bool isEmpty() const { return bar.isEmpty(); }
};

/// Geometry for @p clip drawn in @p clipRect (TimelineLayout::clipRect()).
[[nodiscard]] Geometry geometry(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout);

/// What a press at a position hits.
struct Hit
{
    enum class Kind {
        None,
        Segment, ///< a segment field (opens the speed menu)
        PointTop, ///< a speed point's top handle (drag = slide the point)
        PointBottom, ///< a speed point's retime handle (drag = change the speed before it)
        EndHandle ///< the clip end's retime handle (drag = change the last segment's speed)
    };
    Kind kind = Kind::None;
    int segment = -1; ///< Segment
    int64_t localFrame = 0; ///< PointTop / PointBottom / EndHandle: the point's clip-local frame
};
/// Hit test of @p pos against @p g (handles win over fields).
[[nodiscard]] Hit hitTest(const Geometry& g, QPointF pos, const ClipData& clip);

} // namespace kk::retimebar
