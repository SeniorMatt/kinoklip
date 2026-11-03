// SPDX-License-Identifier: GPL-3.0-or-later
// ScopeFeeder — small renders of the program frame for the scopes.
#include "ui/scopes/ScopeFeeder.h"

#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"

#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <future>
#include <memory>

namespace kk {

ScopeFeeder::ScopeFeeder(PlaybackEngine* engine, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
{
    m_watcher = new QFutureWatcher<RenderedFramePtr>(this);
    connect(m_watcher, &QFutureWatcher<RenderedFramePtr>::finished, this, &ScopeFeeder::onRendered);
    if (engine)
        connect(engine, &PlaybackEngine::frameReady, this, &ScopeFeeder::refresh);
}

void ScopeFeeder::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active)
        refresh();
}

void ScopeFeeder::refresh()
{
    if (!m_active || !m_engine || !m_engine->compositor() || !m_engine->timeline())
        return;
    if (m_inFlight) {
        m_again = true;
        return;
    }
    const RenderedFramePtr current = m_engine->currentFrame();
    const TimelineData& data = m_engine->timeline()->data();
    if (data.width <= 0 || data.height <= 0)
        return;
    const QSize size(
        m_width, std::max(1, int(std::lround(double(m_width) * data.height / data.width))));
    const int64_t frame = current ? current->frame : m_engine->position();
    m_inFlight = true;
    m_again = false;
    auto future
        = std::make_shared<std::future<RenderedFramePtr>>(m_engine->renderImage(frame, size));
    // Wait on a pool thread, deliver on the GUI thread (the compositor's futures are std::).
    m_watcher->setFuture(QtConcurrent::run([future] { return future->get(); }));
}

void ScopeFeeder::onRendered()
{
    m_inFlight = false;
    const RenderedFramePtr frame = m_watcher->result();
    if (frame && frame->ok && !frame->image.isNull()) {
        m_last = frame->image;
        emit imageReady(m_last);
    }
    if (m_again)
        refresh();
}

} // namespace kk
