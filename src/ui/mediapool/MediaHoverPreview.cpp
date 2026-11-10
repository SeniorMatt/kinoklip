// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/mediapool/MediaHoverPreview.h"

#include "media/MediaCache.h"
#include "playback/PlaybackEngine.h"
#include "render/RenderTypes.h"
#include "ui/viewer/ViewerPanel.h"

#include <QGuiApplication>
#include <QImage>

#include <algorithm>
#include <memory>

namespace kk {

MediaHoverPreview::MediaHoverPreview(ViewerPanel* viewer, QObject* parent)
    : QObject(parent)
    , m_viewer(viewer)
{
    m_giveUp.setSingleShot(true);
    m_giveUp.setInterval(kGiveUpMs);
    connect(&m_giveUp, &QTimer::timeout, this, [this] {
        m_inFlight = false;
        update();
    });
    connect(&media::MediaCache::instance(), &media::MediaCache::frameReady, this,
        &MediaHoverPreview::onFrameReady);
    if (viewer) {
        // Loading a clip (a click in the pool) shows that clip, not the hover picture.
        connect(viewer, &ViewerPanel::sourceMediaChanged, this, &MediaHoverPreview::stop);
    }
}

bool MediaHoverPreview::viewerBusy() const
{
    if (!m_viewer || !m_viewer->isVisible())
        return true;
    const PlaybackEngine* engine = m_viewer->engine();
    return engine && engine->isPlaying();
}

void MediaHoverPreview::watchEngine()
{
    PlaybackEngine* engine = m_viewer ? m_viewer->engine() : nullptr;
    if (engine == m_engine)
        return;
    m_engine = engine;
    if (engine) {
        connect(engine, &PlaybackEngine::stateChanged, this, [this] {
            if (m_engine && m_engine->isPlaying())
                stop();
        });
    }
}

void MediaHoverPreview::hover(const QString& path, int64_t durationUs, double position)
{
    watchEngine();
    if (path.isEmpty() || viewerBusy()) {
        stop();
        return;
    }
    position = std::clamp(position, 0.0, 1.0);
    // The last frame sits one frame before the end; 40 ms (25 fps) is close enough.
    const int64_t last = std::max<int64_t>(durationUs - 40000, 0);
    m_path = path;
    m_timeUs = std::min<int64_t>(int64_t(position * double(durationUs)), last);
    update();
}

void MediaHoverPreview::stop()
{
    m_path.clear();
    m_giveUp.stop();
    if (m_showing && m_viewer)
        m_viewer->clearTemporaryFrame();
    m_showing = false;
}

void MediaHoverPreview::update()
{
    if (m_path.isEmpty() || viewerBusy())
        return;
    if (m_inFlight)
        return; // the newest wish is asked for when the request in flight is done
    const QSize size = m_viewer->pictureSize();
    const int height = std::max(180, int(size.height() * m_viewer->devicePixelRatioF()));
    media::MediaCache& cache = media::MediaCache::instance();
    const QImage image = cache.frame(m_path, m_timeUs, height);
    if (image.isNull()) {
        // The cache scheduled the decode; frameReady() follows.
        m_inFlight = true;
        m_flightPath = m_path;
        m_requested.append(m_timeUs);
        m_giveUp.start();
        return;
    }
    auto frame = std::make_shared<RenderedFrame>();
    frame->size = image.size();
    frame->image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    frame->ok = true;
    m_viewer->showTemporaryFrame(frame);
    m_showing = true;
}

void MediaHoverPreview::onFrameReady(const QString& path)
{
    if (!m_inFlight || path != m_flightPath)
        return;
    m_inFlight = false;
    m_giveUp.stop();
    update();
}

} // namespace kk
