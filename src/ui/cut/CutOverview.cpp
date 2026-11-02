// SPDX-License-Identifier: GPL-3.0-or-later
// CutOverview implementation: fitted whole-timeline painting and scrubbing.
#include "ui/cut/CutOverview.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/UiAnimation.h"
#include "ui/timeline/TimelinePainter.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr int kRulerHeight = 14; ///< tick strip at the top
constexpr int kSidePadding = 6; ///< px left and right of the timeline
constexpr double kEndMargin = 0.03; ///< extra width after the last frame (share of the length)
constexpr int kMinSeconds = 10; ///< an empty or short timeline still spans this much
constexpr int kTargetTicks = 12; ///< about this many ruler ticks across the width
constexpr double kAudioShare = 0.35; ///< of the track area for audio (compact)
constexpr int kRowGap = 1;
constexpr int kGlideFrameMs = 16; ///< repaint interval while the playhead glides

/// Tick spacing in seconds: the first "nice" step giving at most kTargetTicks ticks.
int tickSeconds(double totalSeconds)
{
    static constexpr int kSteps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
    for (int s : kSteps) {
        if (totalSeconds / s <= kTargetTicks)
            return s;
    }
    return 7200;
}

} // namespace

CutOverview::CutOverview(AppContext& ctx, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("CutOverview"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(false);
    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, qOverload<>(&QWidget::update));
    connect(timeline, &Timeline::playheadChanged, this, &CutOverview::onPlayheadChanged);
    m_lastPlayhead = timeline->playhead();
    // Playhead glide (v2.2, spec 38.10): repaints while the line glides to a clicked frame.
    m_glideTimer.setTimerType(Qt::PreciseTimer);
    m_glideTimer.setInterval(kGlideFrameMs);
    connect(&m_glideTimer, &QTimer::timeout, this, [this] {
        if (!m_glide.running(uianim::nowMs()))
            m_glideTimer.stop();
        update();
    });
    connect(ctx.project, &Project::activeTimelineChanged, this, qOverload<>(&QWidget::update));
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, this,
        qOverload<>(&QWidget::update));
}

QSize CutOverview::sizeHint() const
{
    return {800, 90};
}

QSize CutOverview::minimumSizeHint() const
{
    return {200, 48};
}

void CutOverview::setDetailRange(double first, double last)
{
    if (first == m_detailFirst && last == m_detailLast)
        return;
    m_detailFirst = first;
    m_detailLast = last;
    update();
}

int64_t CutOverview::visibleFrames() const
{
    const TimelineData& d = m_ctx.project->timeline()->data();
    const int64_t minimum = int64_t(kMinSeconds) * d.fps.num / std::max(1, d.fps.den);
    const int64_t length = d.duration();
    return std::max<int64_t>(minimum, length + int64_t(std::ceil(length * kEndMargin)));
}

double CutOverview::frameToX(double frame) const
{
    const double width = std::max(1, this->width() - 2 * kSidePadding);
    return kSidePadding + frame * width / double(visibleFrames());
}

int64_t CutOverview::xToFrame(double x) const
{
    const double width = std::max(1, this->width() - 2 * kSidePadding);
    const double frame = (x - kSidePadding) * double(visibleFrames()) / width;
    return std::max<int64_t>(0, int64_t(std::floor(frame)));
}

void CutOverview::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    const TimelineData& d = m_ctx.project->timeline()->data();
    const MediaPool* pool = m_ctx.project->mediaPool();
    QPainter p(this);
    p.fillRect(rect(), sc.lane);

    // Ruler with second ticks.
    const QRect ruler(0, 0, width(), kRulerHeight);
    p.fillRect(ruler, sc.ruler);
    const double fps = d.fps.toDouble() > 0 ? d.fps.toDouble() : 25.0;
    const double seconds = double(visibleFrames()) / fps;
    const int step = tickSeconds(seconds);
    p.setPen(sc.grid);
    for (int s = 0; s <= int(seconds); s += step) {
        const int x = int(std::lround(frameToX(s * fps)));
        p.drawLine(x, kRulerHeight - 5, x, kRulerHeight - 1);
    }

    // Tracks: video rows (V(n) at the top) share most of the height, audio rows the rest.
    const int top = kRulerHeight + 2;
    const int area = std::max(0, height() - top - 2);
    const int videoCount = int(d.videoTracks.size());
    const int audioCount = int(d.audioTracks.size());
    const int audioArea = audioCount > 0 ? int(area * kAudioShare) : 0;
    const int videoArea = area - audioArea;
    const double videoRow = videoCount > 0 ? double(videoArea) / videoCount : 0.0;
    const double audioRow = audioCount > 0 ? double(audioArea) / audioCount : 0.0;
    auto paintTrack = [&](const TrackData& track, TrackType type, double y, double h) {
        const QRectF band(0, y, width(), h - kRowGap);
        p.fillRect(band, track.enabled ? sc.laneAlt : sc.lane);
        for (const ClipData& c : track.clips) {
            const MediaItem* item = c.mediaId.isEmpty() ? nullptr : pool->item(c.mediaId);
            QColor fill = TimelinePainter::clipColor(c, type, item);
            if (!c.enabled)
                fill = mixColors(fill, sc.lane, 0.6);
            const double x0 = frameToX(double(c.start));
            const double x1 = std::max(x0 + 1.0, frameToX(double(c.end())));
            const QRectF r(x0, y + 1, x1 - x0, h - kRowGap - 2);
            p.fillRect(r, fill);
            // A thin edge so neighbouring clips stay apart at any zoom.
            p.fillRect(QRectF(x0, r.top(), 1, r.height()), mixColors(fill, sc.shadow, 0.5));
        }
    };
    for (int i = videoCount - 1; i >= 0; --i) {
        const double y = top + (videoCount - 1 - i) * videoRow;
        paintTrack(d.videoTracks[size_t(i)], TrackType::Video, y, videoRow);
    }
    for (int i = 0; i < audioCount; ++i) {
        const double y = top + videoArea + i * audioRow;
        paintTrack(d.audioTracks[size_t(i)], TrackType::Audio, y, audioRow);
    }

    // The lower timeline's range.
    if (m_detailLast > m_detailFirst) {
        const double x0 = frameToX(m_detailFirst);
        const double x1 = frameToX(m_detailLast);
        const QRectF band(x0, 1, std::max(2.0, x1 - x0), height() - 2);
        p.fillRect(band, withAlpha(sc.focus, 40));
        p.setPen(QPen(sc.focus, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(band);
    }

    // In / out marks and the playhead.
    p.setPen(QPen(sc.playhead, 1, Qt::DashLine));
    for (const int64_t mark : {d.markIn, d.markOut}) {
        if (mark >= 0) {
            const int x = int(std::lround(frameToX(double(mark))));
            p.drawLine(x, 0, x, height());
        }
    }
    const double px
        = frameToX(m_glide.frame(m_ctx.project->timeline()->playhead(), uianim::nowMs()));
    p.setPen(QPen(sc.playhead, 2));
    p.drawLine(QPointF(px, 0), QPointF(px, height()));
}

void CutOverview::onPlayheadChanged(int64_t frame)
{
    // A click glides the line (isolated jump); dragging and playback stay 1:1.
    const bool playing = m_ctx.playback && m_ctx.playback->isPlaying();
    const double pxPerFrame
        = (width() - 2.0 * kSidePadding) / double(std::max<int64_t>(1, visibleFrames()));
    if (m_glide.moved(m_lastPlayhead, frame, uianim::enabled() && !playing, pxPerFrame)
        && !m_glideTimer.isActive())
        m_glideTimer.start();
    m_lastPlayhead = frame;
    update();
}

void CutOverview::scrubTo(double x)
{
    m_ctx.project->timeline()->setPlayhead(xToFrame(x));
}

void CutOverview::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(event);
    m_scrubbing = true;
    scrubTo(event->position().x());
}

void CutOverview::mouseMoveEvent(QMouseEvent* event)
{
    if (m_scrubbing)
        scrubTo(event->position().x());
}

void CutOverview::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        m_scrubbing = false;
}

} // namespace kk
