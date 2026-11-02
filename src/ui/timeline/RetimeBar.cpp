// SPDX-License-Identifier: GPL-3.0-or-later
// RetimeBar implementation (see RetimeBar.h).
#include "ui/timeline/RetimeBar.h"

#include <algorithm>

namespace kk::retimebar {

Geometry geometry(const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout)
{
    Geometry g;
    if (clipRect.height() < kMinClipHeight || clipRect.width() < 8)
        return g;
    g.bar = QRectF(clipRect.left(), clipRect.top() + kNameBand, clipRect.width(), kBarHeight);
    const QVector<retime::Segment> segs = retime::segments(clip);
    for (int i = 0; i < segs.size(); ++i) {
        const double x0 = layout.frameToX(double(clip.start + segs[i].startLocal));
        const double x1 = layout.frameToX(double(clip.start + segs[i].endLocal));
        SegmentField f;
        f.index = i;
        f.segment = segs[i];
        f.rect = QRectF(QPointF(std::max(x0, g.bar.left()), g.bar.top()),
            QPointF(std::min(x1, g.bar.right()), g.bar.bottom()));
        // The arrow sits left of the boundary handle (speed point / clip end) at the field's
        // right edge.
        const double arrowRight = f.rect.right() - kHandleHalfWidth;
        const double arrowW = std::clamp(arrowRight - f.rect.left(), 0.0, kArrowWidth);
        f.arrow = QRectF(arrowRight - arrowW, f.rect.top(), arrowW, f.rect.height());
        g.fields.append(f);
    }
    for (int64_t local : retime::speedPoints(clip)) {
        PointMarks m;
        m.localFrame = local;
        m.x = layout.frameToX(double(clip.start + local));
        m.top = QRectF(m.x - kHandleHalfWidth, g.bar.top(), 2 * kHandleHalfWidth, g.bar.height());
        m.bottom = QRectF(m.x - kHandleHalfWidth, clipRect.bottom() - kBottomHandleHeight,
            2 * kHandleHalfWidth, kBottomHandleHeight);
        g.points.append(m);
    }
    // At the clip end inside the bar (the bottom-right corner holds the keyframe buttons).
    g.endHandle = QRectF(
        clipRect.right() - kHandleHalfWidth, g.bar.top(), kHandleHalfWidth, g.bar.height());
    return g;
}

Hit hitTest(const Geometry& g, QPointF pos, const ClipData& clip)
{
    Hit h;
    if (g.isEmpty())
        return h;
    for (const PointMarks& m : g.points) {
        if (m.top.contains(pos)) {
            h.kind = Hit::Kind::PointTop;
            h.localFrame = m.localFrame;
            return h;
        }
        if (m.bottom.contains(pos)) {
            h.kind = Hit::Kind::PointBottom;
            h.localFrame = m.localFrame;
            return h;
        }
    }
    if (g.endHandle.contains(pos)) {
        h.kind = Hit::Kind::EndHandle;
        h.localFrame = clip.duration;
        return h;
    }
    for (const SegmentField& f : g.fields) {
        if (f.rect.contains(pos)) {
            h.kind = Hit::Kind::Segment;
            h.segment = f.index;
            return h;
        }
    }
    return h;
}

} // namespace kk::retimebar
