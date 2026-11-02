// SPDX-License-Identifier: GPL-3.0-or-later
// AutoQuality — conservative Full/Half decision for the "Automatic" proxy resolution.
#include "playback/AutoQuality.h"

#include <algorithm>

namespace kk {

void AutoQuality::reset(int64_t nowUs)
{
    m_quality = Quality::Full;
    m_changedUs = nowUs;
    m_windowStartUs = nowUs;
    m_overloaded = 0;
    m_headroomSinceUs = -1;
    m_fullLoad = 0.0;
    m_backoffUs = 0;
    m_upshiftNotBeforeUs = 0;
    m_upshiftedUs = -1;
    m_stayFull = false;
    m_lastLoad = 0.0;
}

bool AutoQuality::change(Quality next, int64_t nowUs)
{
    m_quality = next;
    m_changedUs = nowUs;
    m_windowStartUs = nowUs;
    m_overloaded = 0;
    m_headroomSinceUs = -1;
    return true;
}

bool AutoQuality::update(int64_t nowUs, double avgRenderMs, double intervalMs)
{
    if (nowUs - m_changedUs < kSettleUs || nowUs - m_windowStartUs < kWindowUs)
        return false;
    m_windowStartUs = nowUs;
    if (avgRenderMs <= 0.0 || intervalMs <= 0.0)
        return false;
    const double load = avgRenderMs / intervalMs;
    m_lastLoad = load;

    if (m_quality == Quality::Full) {
        if (load <= kDownshiftLoad) {
            m_overloaded = 0;
            return false;
        }
        if (++m_overloaded < kOverloadWindows || m_stayFull)
            return false;
        // Full failed again soon after an upshift: wait longer before the next try.
        if (m_upshiftedUs >= 0 && nowUs - m_upshiftedUs < kMaxBackoffUs)
            m_backoffUs = m_backoffUs == 0 ? kBackoffUs : std::min(m_backoffUs * 2, kMaxBackoffUs);
        m_upshiftNotBeforeUs = nowUs + m_backoffUs;
        m_fullLoad = load;
        return change(Quality::Half, nowUs);
    }

    // At Half (Auto never goes lower).
    const bool useless = m_fullLoad > 0.0 && load > m_fullLoad * kUsefulSpeedup;
    if (useless) {
        // The bottleneck is not the picture size (decoding, uploads): Half only costs
        // sharpness. Stay at Full for the rest of this play.
        m_stayFull = true;
        m_fullLoad = 0.0;
        return change(Quality::Full, nowUs);
    }
    if (load >= kUpshiftLoad) {
        m_headroomSinceUs = -1;
        return false;
    }
    if (m_headroomSinceUs < 0)
        m_headroomSinceUs = nowUs;
    if (nowUs - m_headroomSinceUs < kUpshiftHoldUs || nowUs < m_upshiftNotBeforeUs)
        return false;
    m_upshiftedUs = nowUs;
    return change(Quality::Full, nowUs);
}

} // namespace kk
