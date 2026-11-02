// SPDX-License-Identifier: GPL-3.0-or-later
// DragInfo — the small label next to the pointer while a clip is moved or trimmed (offset and
// the new timecode / duration), and the frames of a trim for the viewer's two-up view.
#pragma once

#include "core/Rational.h"

#include <QPointF>
#include <QStringList>

#include <cstdint>

class QFont;
class QPainter;
class QRect;

namespace kk {

/// Text drawn next to the pointer during a gesture (spec 21.9); empty lines = none.
struct DragInfoLabel
{
    QStringList lines; ///< first line: the offset; later lines: details (dimmer)
    QPointF anchor; ///< pointer position in view coordinates
    [[nodiscard]] bool isEmpty() const { return lines.isEmpty(); }
};

/// One side of an edit shown by the two-up trim view (spec 8.15).
struct TrimPreviewSide
{
    quint64 clipId = 0; ///< 0 = nothing on this side (gap, timeline edge): drawn black
    QString mediaId; ///< media of the clip; empty for titles / generators
    int64_t sourceUs = 0; ///< media time of the frame shown
    int64_t timelineFrame = 0; ///< the frame's position on the timeline
    QString name; ///< clip name
};

/// What the two-up trim view shows: the outgoing (left) and incoming (right) frame of the edit.
struct TrimPreview
{
    TrimPreviewSide left;
    TrimPreviewSide right;
    [[nodiscard]] bool isActive() const { return left.clipId != 0 || right.clipId != 0; }
    bool operator==(const TrimPreview& o) const
    {
        return left.clipId == o.left.clipId && left.sourceUs == o.left.sourceUs
            && left.timelineFrame == o.left.timelineFrame && right.clipId == o.right.clipId
            && right.sourceUs == o.right.sourceUs && right.timelineFrame == o.right.timelineFrame;
    }
};

namespace draginfo {

/// Signed timecode of an offset: "+00:00:01:05", "-00:00:00:12" (zero = "+00:00:00:00").
[[nodiscard]] QString formatOffset(int64_t frames, Rational fps);

/// Draws @p label as a plate below-right of its anchor, kept inside @p bounds. Colours come
/// from SchemeColors (overlay plate and text).
void paint(QPainter& p, const DragInfoLabel& label, const QFont& font, const QRect& bounds);

} // namespace draginfo

} // namespace kk
