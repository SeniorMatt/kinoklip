// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeRuler implementation: ticks, timecodes, clip spans, playhead, scrubbing.
#include "ui/keyframes/KeyframeRuler.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/keyframes/KeyframeGrid.h"
#include "ui/keyframes/KeyframeTimeMap.h"
#include "ui/keyframes/KeyframeViewBase.h"

#include <QMouseEvent>
#include <QPainter>

#include <cmath>

namespace kk {

namespace {
constexpr int kRulerHeight = 24;
constexpr double kLabelSpacingPx = 90.0; ///< minimum distance of labelled ticks
constexpr int kMinorTicks = 5; ///< minor ticks per labelled step
constexpr int kClipSpanAlpha = 90; ///< shown clips: the video clip colour, translucent
} // namespace

KeyframeRuler::KeyframeRuler(AppContext& ctx, const KeyframeTimeMap& map, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
    , m_map(map)
{
    setFixedHeight(kRulerHeight);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::PointingHandCursor);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, [this] { update(); });
    connect(ctx.project->timeline(), &Timeline::changed, this, [this] { update(); });
}

QSize KeyframeRuler::sizeHint() const
{
    return QSize(200, kRulerHeight);
}

void KeyframeRuler::setLabelWidth(int px)
{
    m_labelWidth = px;
    update();
}

void KeyframeRuler::setClips(const QVector<quint64>& clipIds)
{
    m_clips = clipIds;
    update();
}

void KeyframeRuler::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QPalette& pal = palette();
    const QColor text = pal.color(QPalette::WindowText);
    const QColor dim = pal.color(QPalette::PlaceholderText);
    p.fillRect(rect(), schemeColors().keyRuler);
    p.setPen(pal.color(QPalette::Dark));
    p.drawLine(0, height() - 1, width(), height() - 1);
    if (m_labelWidth > 0)
        p.drawLine(m_labelWidth - 1, 0, m_labelWidth - 1, height());
    p.setClipRect(QRect(m_labelWidth, 0, width() - m_labelWidth, height()));

    const TimelineData& data = m_ctx.project->timeline()->data();
    // Spans of the shown clips along the bottom edge.
    for (quint64 id : std::as_const(m_clips)) {
        if (const ClipData* c = editops::clipById(data, id)) {
            const double x0 = m_map.frameToX(double(c->start));
            const double x1 = m_map.frameToX(double(c->start + c->duration));
            p.fillRect(QRectF(x0, height() - 6, x1 - x0, 5),
                withAlpha(schemeColors().videoTrack, kClipSpanAlpha));
        }
    }

    // Ticks: a labelled step at least kLabelSpacingPx apart, minor ticks in between.
    const double ppf = m_map.pixelsPerFrame();
    const int64_t step = keyframegrid::timeStepFrames(data.fps, ppf, kLabelSpacingPx);
    const int64_t minor = std::max<int64_t>(1, step / kMinorTicks);
    const double f0 = m_map.xToFrame(m_labelWidth);
    const double f1 = m_map.xToFrame(width());
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() - 1.0));
    p.setFont(small);
    if (minor * ppf >= 4.0) {
        p.setPen(dim);
        for (int64_t f = std::max<int64_t>(0, int64_t(std::floor(f0 / double(minor))) * minor);
             double(f) <= f1; f += minor) {
            const double x = std::round(m_map.frameToX(double(f))) + 0.5;
            p.drawLine(QPointF(x, height() - 4), QPointF(x, height() - 1));
        }
    }
    for (int64_t f = std::max<int64_t>(0, int64_t(std::floor(f0 / double(step))) * step);
         double(f) <= f1; f += step) {
        const double x = std::round(m_map.frameToX(double(f))) + 0.5;
        p.setPen(dim);
        p.drawLine(QPointF(x, height() - 9), QPointF(x, height() - 1));
        if (x + 3 < m_labelWidth)
            continue; // the label would be cut by the axis column
        p.setPen(text);
        p.drawText(QPointF(x + 3, height() - 10), timecode::formatRecord(f, data));
    }

    // Playhead: a marker on the ruler (the views draw the line).
    const double px
        = std::round(m_map.frameToX(double(m_ctx.project->timeline()->playhead()))) + 0.5;
    if (px >= m_labelWidth - 6 && px <= width() + 6) {
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPointF tri[3] = {{px - 5, 0}, {px + 5, 0}, {px, 7}};
        p.setPen(Qt::NoPen);
        p.setBrush(schemeColors().playhead);
        p.drawConvexPolygon(tri, 3);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(schemeColors().playhead);
        p.drawLine(QPointF(px, 0), QPointF(px, height()));
    }
}

void KeyframeRuler::scrubTo(double x)
{
    int64_t frame
        = std::max<int64_t>(0, std::llround(m_map.xToFrame(std::max<double>(x, m_labelWidth))));
    // Read live: N during the drag turns snapping off / on (spec 11.5).
    if (KeyframeViewBase::snappingAction(m_ctx)) {
        const auto tolerance = int64_t(editops::kSnapDistancePx / m_map.pixelsPerFrame());
        if (const auto snapped = editops::snapToNearest(m_snaps, frame, tolerance))
            frame = *snapped;
    }
    m_ctx.project->timeline()->setPlayhead(frame);
}

void KeyframeRuler::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_scrubbing = true;
    const TimelineData& data = m_ctx.project->timeline()->data();
    m_snaps = keyframeops::withKeySnapTargets(editops::snapCandidates(data, -1), data, m_clips);
    scrubTo(event->position().x());
}

void KeyframeRuler::mouseMoveEvent(QMouseEvent* event)
{
    if (m_scrubbing)
        scrubTo(event->position().x());
}

void KeyframeRuler::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        m_scrubbing = false;
}

} // namespace kk
