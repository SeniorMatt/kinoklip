// SPDX-License-Identifier: GPL-3.0-or-later
// FastReview implementation.
#include "ui/cut/FastReview.h"

#include "core/CutEdits.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"

#include <cmath>

namespace kk {

FastReview::FastReview(AppContext& ctx, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_engine(ctx.playback)
{
    if (m_engine) {
        connect(m_engine, &PlaybackEngine::positionChanged, this, &FastReview::onPosition);
        connect(m_engine, &PlaybackEngine::rateChanged, this, &FastReview::onRate);
    }
}

double FastReview::rateAt(int64_t frame) const
{
    const TimelineData& d = m_ctx.project->timeline()->data();
    const int64_t start = editops::previousEditPoint(d, frame + 1).value_or(0);
    const int64_t end = editops::nextEditPoint(d, frame).value_or(d.duration());
    return cutedits::fastReviewRate(std::max<int64_t>(1, end - start), d.fps);
}

void FastReview::setActive(bool active)
{
    if (active)
        start();
    else
        stop();
}

void FastReview::start()
{
    if (m_active || !m_engine || m_ctx.project->timeline()->data().duration() <= 0) {
        emit activeChanged(m_active);
        return;
    }
    m_active = true;
    m_shotStart = m_shotEnd = -1;
    emit activeChanged(true);
    onPosition(m_engine->position());
}

void FastReview::stop()
{
    if (!m_active)
        return;
    finish();
    if (m_engine)
        m_engine->stop();
}

void FastReview::finish()
{
    m_active = false;
    m_rate = 0.0;
    emit activeChanged(false);
}

void FastReview::onPosition(int64_t frame)
{
    if (!m_active || !m_engine)
        return;
    if (frame >= m_shotStart && frame < m_shotEnd && m_engine->rate() == m_rate)
        return; // same shot, rate already set
    const TimelineData& d = m_ctx.project->timeline()->data();
    if (frame >= d.duration() - 1 && m_engine->rate() == 0.0 && m_rate != 0.0) {
        finish(); // reached the end
        return;
    }
    m_shotStart = editops::previousEditPoint(d, frame + 1).value_or(0);
    m_shotEnd = editops::nextEditPoint(d, frame).value_or(d.duration());
    m_rate = cutedits::fastReviewRate(std::max<int64_t>(1, m_shotEnd - m_shotStart), d.fps);
    m_engine->setRate(m_rate);
}

void FastReview::onRate(double rate)
{
    if (!m_active)
        return;
    // A rate the review did not set (K, Space, J/L, the timeline end): the user took over.
    if (rate != m_rate)
        finish();
}

} // namespace kk
