// SPDX-License-Identifier: GPL-3.0-or-later
// TrimTwoUpView — two-up trim pictures in the timeline viewer.
#include "ui/viewer/TrimTwoUpView.h"

#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Timeline.h"
#include "media/MediaCache.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/viewer/ViewerSurface.h"

#include <QEvent>
#include <QPainter>
#include <QTimer>

#include <algorithm>

namespace kk {

namespace {
/// Gap between the two pictures and around them (px).
constexpr int kGap = 6;
/// Height of the caption band under each picture (name + timecode).
constexpr int kCaption = 34;
/// Requests are made at the displayed size but not larger than this (decode cost).
constexpr int kMaxRequestHeight = 540;
/// How often finished Program renders are collected while one is in flight (ms).
constexpr int kRenderPollMs = 4;

/// A one-clip timeline showing @p clip of @p snapshot with its own look (effects, transform,
/// crop, masks) but without its transitions and fades, so the edit frame is not faded out.
std::shared_ptr<const TimelineData> clipAlone(const TimelineData& snapshot, const ClipData& clip)
{
    auto data = std::make_shared<TimelineData>();
    data->fps = snapshot.fps;
    data->width = snapshot.width;
    data->height = snapshot.height;
    data->retimeProcess = snapshot.retimeProcess;
    data->nested = snapshot.nested; // nested clips render their timelines
    TrackData track;
    track.id = 1;
    track.type = TrackType::Video;
    ClipData c = clip;
    c.transitionIn.reset();
    c.transitionOut.reset();
    c.fadeInFrames = 0;
    c.fadeInCurve = 0.0;
    c.fadeOutFrames = 0;
    c.fadeOutCurve = 0.0;
    c.enabled = true;
    track.clips.push_back(std::move(c));
    data->videoTracks.push_back(std::move(track));
    return data;
}
} // namespace

TrimTwoUpView::TrimTwoUpView(ViewerSurface* surface, const MediaPool* pool)
    : QWidget(surface->widget())
    , m_surface(surface)
    , m_pool(pool)
{
    setObjectName(QStringLiteral("TrimTwoUpView"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_TransparentForMouseEvents); // the drag happens in the timeline
    surface->widget()->installEventFilter(this);
    setGeometry(surface->widget()->rect());
    connect(&media::MediaCache::instance(), &media::MediaCache::frameReady, this,
        &TrimTwoUpView::onFrameReady);
    m_poll = new QTimer(this);
    m_poll->setInterval(kRenderPollMs);
    connect(m_poll, &QTimer::timeout, this, &TrimTwoUpView::collectRenders);
    hide();
}

void TrimTwoUpView::setProgramSource(PlaybackEngine* engine, Timeline* timeline)
{
    m_engine = engine;
    m_timeline = timeline;
}

void TrimTwoUpView::setPictures(Pictures pictures)
{
    m_pictures = pictures;
}

TrimTwoUpView::Pictures TrimTwoUpView::pictures() const
{
    const bool gpu
        = m_engine && m_timeline && m_engine->compositor() && m_engine->compositor()->isGpu();
    return m_pictures == Pictures::Program && gpu ? Pictures::Program : Pictures::Source;
}

void TrimTwoUpView::setPreview(const TrimPreview& preview, Rational fps)
{
    m_preview = preview;
    m_fps = fps;
    if (!preview.isActive()) {
        hide();
        for (Side& s : m_sides)
            s = Side {};
        return;
    }
    const TrimPreviewSide* sides[2] = {&preview.left, &preview.right};
    if (pictures() == Pictures::Program) {
        for (int i = 0; i < 2; ++i) {
            Side& s = m_sides[i];
            s.path.clear();
            s.wantedClip = sides[i]->clipId;
            s.wantedFrame = sides[i]->timelineFrame;
            s.wantedRevision = m_timeline->revision();
            if (s.wantedClip == 0)
                s.image = QImage();
            renderSide(i);
        }
        refresh();
        show();
        raise();
        update();
        return;
    }
    for (int i = 0; i < 2; ++i) {
        Side& s = m_sides[i];
        const MediaItem* item
            = sides[i]->mediaId.isEmpty() || !m_pool ? nullptr : m_pool->item(sides[i]->mediaId);
        const QString path = item ? item->path : QString();
        if (path != s.path) {
            s = Side {};
            s.path = path;
        }
        s.wantedUs = path.isEmpty() ? -1 : sides[i]->sourceUs;
        fetch(i);
    }
    refresh();
    show();
    raise();
    update();
}

bool TrimTwoUpView::hasPicture(int side) const
{
    const Side& s = m_sides[std::clamp(side, 0, 1)];
    if (pictures() == Pictures::Program)
        return !s.image.isNull() && s.shownClip == s.wantedClip && s.shownFrame == s.wantedFrame
            && s.shownRevision == s.wantedRevision;
    return !s.image.isNull() && s.shownUs == s.wantedUs;
}

void TrimTwoUpView::renderSide(int i)
{
    Side& s = m_sides[i];
    if (s.rendering || s.wantedClip == 0)
        return; // one render in flight per side; the newest wish follows when it is done
    if (s.shownClip == s.wantedClip && s.shownFrame == s.wantedFrame
        && s.shownRevision == s.wantedRevision && !s.image.isNull())
        return;
    const std::shared_ptr<const TimelineData> snapshot = m_timeline->snapshot();
    const ClipData* clip = editops::clipById(*snapshot, s.wantedClip);
    if (!clip)
        return;
    RenderRequest request;
    request.timeline = clipAlone(*snapshot, *clip);
    request.media = m_engine->mediaPaths();
    request.proxies = m_engine->proxies(); // proxies decode faster, like the viewer
    request.frame = s.wantedFrame;
    const int h = requestHeight();
    const double aspect = snapshot->height > 0 ? double(snapshot->width) / snapshot->height : 1.0;
    request.outSize = QSize(std::max(1, int(h * aspect)), h);
    request.outputs = OutputImage;
    request.subtitles = false;
    s.future = m_engine->compositor()->renderAsync(std::move(request));
    s.rendering = true;
    s.renderingClip = s.wantedClip;
    s.renderingFrame = s.wantedFrame;
    s.renderingRevision = s.wantedRevision;
    if (!m_poll->isActive())
        m_poll->start();
}

void TrimTwoUpView::collectRenders()
{
    bool busy = false;
    for (int i = 0; i < 2; ++i) {
        Side& s = m_sides[i];
        if (!s.rendering)
            continue;
        if (s.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            busy = true;
            continue;
        }
        const RenderedFramePtr frame = s.future.get();
        s.rendering = false;
        if (frame && frame->ok && !frame->image.isNull()) {
            s.image = frame->image;
            s.shownClip = s.renderingClip;
            s.shownFrame = s.renderingFrame;
            s.shownRevision = s.renderingRevision;
        }
        if (isVisible() && pictures() == Pictures::Program) {
            renderSide(i); // the drag moved on meanwhile
            busy |= s.rendering;
        }
        update();
    }
    if (!busy)
        m_poll->stop();
}

void TrimTwoUpView::refresh()
{
    setGeometry(m_surface->widget()->rect());
    update();
}

int TrimTwoUpView::requestHeight() const
{
    return std::clamp(height() - 2 * kGap - kCaption, 64, kMaxRequestHeight);
}

void TrimTwoUpView::fetch(int i)
{
    Side& s = m_sides[i];
    if (s.path.isEmpty() || s.wantedUs < 0)
        return;
    media::MediaCache& cache = media::MediaCache::instance();
    // One request in flight per side: while one decodes, only remember the newest wish.
    if (s.pendingUs >= 0 && s.pendingUs != s.wantedUs)
        return;
    const QImage image = cache.frame(s.path, s.wantedUs, requestHeight());
    if (image.isNull()) {
        s.pendingUs = s.wantedUs;
        return;
    }
    s.pendingUs = -1;
    s.image = image;
    s.shownUs = s.wantedUs;
}

void TrimTwoUpView::onFrameReady(const QString& path)
{
    if (!isVisible())
        return;
    for (int i = 0; i < 2; ++i) {
        Side& s = m_sides[i];
        if (s.path != path || s.pendingUs < 0)
            continue;
        // Pick up the finished frame, then ask for the newest wish if it moved on meanwhile.
        const QImage image
            = media::MediaCache::instance().frame(path, s.pendingUs, requestHeight());
        if (!image.isNull()) {
            s.image = image;
            s.shownUs = s.pendingUs;
        }
        s.pendingUs = -1;
        if (s.shownUs != s.wantedUs)
            fetch(i);
    }
    update();
}

bool TrimTwoUpView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_surface->widget() && event->type() == QEvent::Resize)
        refresh();
    return QWidget::eventFilter(watched, event);
}

void TrimTwoUpView::paintSide(
    QPainter& p, const QRect& r, const TrimPreviewSide& side, const Side& s)
{
    const SchemeColors& c = schemeColors();
    const QRect picture(r.left(), r.top(), r.width(), r.height() - kCaption);
    // Video content stays black behind the frame (fixed, like the viewer letterbox).
    p.fillRect(picture, Qt::black);
    if (!s.image.isNull()) {
        const QSize fitted = s.image.size().scaled(picture.size(), Qt::KeepAspectRatio);
        const QRect target(picture.left() + (picture.width() - fitted.width()) / 2,
            picture.top() + (picture.height() - fitted.height()) / 2, fitted.width(),
            fitted.height());
        p.drawImage(target, s.image);
    } else if (side.clipId != 0) {
        p.setPen(c.viewerText);
        p.drawText(picture, Qt::AlignCenter, side.name);
    }
    const QRect caption(r.left(), picture.bottom() + 1, r.width(), kCaption);
    p.setPen(c.viewerText);
    const QFontMetrics fm(font());
    if (side.clipId != 0) {
        p.drawText(caption.adjusted(2, 2, -2, 0), Qt::AlignLeft | Qt::AlignTop,
            fm.elidedText(side.name, Qt::ElideRight, caption.width() - 4));
        p.drawText(caption.adjusted(2, 0, -2, -2), Qt::AlignLeft | Qt::AlignBottom,
            timecode::format(
                side.timelineFrame + (m_timeline ? m_timeline->data().startTimecode : 0), m_fps));
    }
}

void TrimTwoUpView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const SchemeColors& c = schemeColors();
    p.fillRect(rect(), c.viewerSurround);
    const int w = (width() - 3 * kGap) / 2;
    const int h = height() - 2 * kGap;
    const QRect left(kGap, kGap, w, h);
    const QRect right(2 * kGap + w, kGap, w, h);
    paintSide(p, left, m_preview.left, m_sides[0]);
    paintSide(p, right, m_preview.right, m_sides[1]);
    // The edit between them.
    p.fillRect(QRect(left.right() + kGap / 2 - 1, kGap, 2, h - kCaption), c.snap);
}

} // namespace kk
