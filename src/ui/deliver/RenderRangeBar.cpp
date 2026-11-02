// SPDX-License-Identifier: GPL-3.0-or-later
// RenderRangeBar — painting and seeking.
#include "ui/deliver/RenderRangeBar.h"

#include "core/Timeline.h"
#include "ui/common/SchemeColors.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace kk {

namespace {
constexpr int kMargin = 6; ///< horizontal padding in px
constexpr int kLaneGap = 1;
constexpr int64_t kMinVisibleFrames = 25; ///< an empty timeline still shows a ruler of 1 s
} // namespace

RenderRangeBar::RenderRangeBar(Timeline* timeline, QWidget* parent)
    : QWidget(parent)
    , m_timeline(timeline)
{
    setObjectName(QStringLiteral("RenderRangeBar"));
    setToolTip(tr("Click or drag to choose the frame shown in the viewer."));
    connect(m_timeline, &Timeline::changed, this, qOverload<>(&QWidget::update));
    connect(m_timeline, &Timeline::playheadChanged, this, qOverload<>(&QWidget::update));
}

void RenderRangeBar::setRange(int64_t first, int64_t end)
{
    m_first = first;
    m_end = end;
    update();
}

int64_t RenderRangeBar::visibleFrames() const
{
    return std::max(kMinVisibleFrames, m_timeline->data().duration());
}

double RenderRangeBar::frameToX(int64_t frame) const
{
    const double w = std::max(1, width() - 2 * kMargin);
    return kMargin + double(frame) * w / double(visibleFrames());
}

void RenderRangeBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QPalette& pal = palette();
    // Same roles as the timeline: clip colours, in/out range in focus, playhead in negative.
    const SchemeColors& sc = schemeColors();
    p.fillRect(rect(), pal.color(QPalette::Base));
    const TimelineData& tl = m_timeline->data();
    const int lanes = int(tl.videoTracks.size() + tl.audioTracks.size());
    const int top = 4;
    const int laneHeight = lanes > 0 ? std::max(3, (height() - 2 * top) / lanes - kLaneGap) : 0;

    // Render range: tinted background across all lanes.
    if (m_end > m_first) {
        const QColor range = withAlpha(sc.focus, 35);
        p.fillRect(
            QRectF(frameToX(m_first), 0, frameToX(m_end) - frameToX(m_first), height()), range);
    }

    // Lanes: video tracks top-down (highest track first, like the timeline), then audio.
    int y = top;
    auto drawTrack = [&](const TrackData& track, const QColor& colour) {
        for (const ClipData& clip : track.clips) {
            const double x0 = frameToX(clip.start);
            const double x1 = std::max(x0 + 1.0, frameToX(clip.end()));
            p.fillRect(QRectF(x0, y, x1 - x0 - 0.5, laneHeight),
                clip.enabled ? colour : colour.darker(200));
        }
        y += laneHeight + kLaneGap;
    };
    for (auto it = tl.videoTracks.rbegin(); it != tl.videoTracks.rend(); ++it) {
        drawTrack(*it, sc.videoClip);
    }
    for (const TrackData& track : tl.audioTracks) {
        drawTrack(track, sc.audioClip);
    }

    // Range edges and playhead.
    if (m_end > m_first) {
        p.setPen(QPen(sc.focus, 2));
        p.drawLine(QPointF(frameToX(m_first), 0), QPointF(frameToX(m_first), height()));
        p.drawLine(QPointF(frameToX(m_end), 0), QPointF(frameToX(m_end), height()));
    }
    p.setPen(QPen(sc.playhead, 1.5));
    const double ph = frameToX(m_timeline->playhead());
    p.drawLine(QPointF(ph, 0), QPointF(ph, height()));
}

void RenderRangeBar::seekTo(int x)
{
    const double w = std::max(1, width() - 2 * kMargin);
    const int64_t frame = int64_t(double(x - kMargin) * double(visibleFrames()) / w);
    m_timeline->setPlayhead(
        std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, m_timeline->data().duration() - 1)));
}

void RenderRangeBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        seekTo(int(event->position().x()));
    }
}

void RenderRangeBar::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() & Qt::LeftButton) {
        seekTo(int(event->position().x()));
    }
}

} // namespace kk
