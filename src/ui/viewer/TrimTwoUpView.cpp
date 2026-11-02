// SPDX-License-Identifier: GPL-3.0-or-later
// TrimTwoUpView — two-up trim pictures in the timeline viewer.
#include "ui/viewer/TrimTwoUpView.h"

#include "core/MediaPool.h"
#include "media/MediaCache.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/viewer/ViewerSurface.h"

#include <QEvent>
#include <QPainter>

#include <algorithm>

namespace kk {

namespace {
/// Gap between the two pictures and around them (px).
constexpr int kGap = 6;
/// Height of the caption band under each picture (name + timecode).
constexpr int kCaption = 34;
/// Requests are made at the displayed size but not larger than this (decode cost).
constexpr int kMaxRequestHeight = 540;
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
    hide();
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
    return !s.image.isNull() && s.shownUs == s.wantedUs;
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
            timecode::format(side.timelineFrame, m_fps));
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
