// SPDX-License-Identifier: GPL-3.0-or-later
// ScopeFeeder — supplies the video scopes with small pictures of the program frame: whenever
// the viewer shows a new frame (playback, scrubbing, an edit, a grade change) it renders the
// same frame again as a small image (PlaybackEngine::renderImage(), what the viewer shows,
// graded) and hands it out. At most one render in flight; requests while it runs collapse
// into one more of the newest frame.
#pragma once

#include "render/RenderTypes.h"

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QPointer>

namespace kk {

class PlaybackEngine;

class ScopeFeeder : public QObject
{
    Q_OBJECT
public:
    /// Feeds from @p engine (null: never emits). Starts inactive.
    explicit ScopeFeeder(PlaybackEngine* engine, QObject* parent = nullptr);

    /// Active feeders render on every new frame; inactive ones do nothing (hidden scopes).
    void setActive(bool active);
    [[nodiscard]] bool isActive() const { return m_active; }
    /// Width of the rendered pictures (default 480; the height follows the timeline aspect).
    void setWidth(int width) { m_width = width; }
    /// Renders the current frame now (an edit that does not change the viewer's frame, e.g. a
    /// grade change while paused, still re-renders through frameReady).
    void refresh();
    /// The last picture handed out.
    [[nodiscard]] const QImage& lastImage() const { return m_last; }

Q_SIGNALS:
    void imageReady(const QImage& image);

private:
    void onRendered();

    QPointer<PlaybackEngine> m_engine;
    QFutureWatcher<RenderedFramePtr>* m_watcher = nullptr;
    QImage m_last;
    int m_width = 480;
    bool m_active = false;
    bool m_inFlight = false;
    bool m_again = false;
};

} // namespace kk
