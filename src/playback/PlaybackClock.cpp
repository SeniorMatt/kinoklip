// SPDX-License-Identifier: GPL-3.0-or-later
// PlaybackClock — free-running and audio-slaved playback clock.
#include "playback/PlaybackClock.h"

#include <algorithm>
#include <cmath>

namespace kk {

void PlaybackClock::startFree(double startFrame, double rate, int64_t nowUs)
{
    m_running = true;
    m_audio = false;
    m_audioStarted = false;
    m_rate = rate;
    m_startFrame = m_baseFrame = m_last = startFrame;
    m_lastReportUs = -2;
    m_baseUs = nowUs;
    m_lastError = 0.0;
}

void PlaybackClock::startAudio(double startFrame, int64_t nowUs)
{
    startFree(startFrame, 1.0, nowUs);
    m_audio = true;
}

double PlaybackClock::position(int64_t nowUs, int64_t audioPlayedFrames, int64_t audioSampleTimeUs)
{
    if (!m_running) {
        return m_last;
    }
    const double framesPerUs = m_fps * 1e-6;
    if (m_audio) {
        if (audioPlayedFrames <= 0) {
            // Device not started yet (or nothing known): hold, and keep the wall-clock base at
            // "now" so the clock does not jump ahead once the first samples are heard.
            if (!m_audioStarted) {
                m_baseUs = nowUs;
                return m_last;
            }
        } else if (audioSampleTimeUs != m_lastReportUs || !m_audioStarted) {
            // A new device report. Its position, extrapolated to nowUs over the time since it
            // was measured (at most one audio period).
            m_lastReportUs = audioSampleTimeUs;
            const int64_t sampleUs = audioSampleTimeUs >= 0 ? audioSampleTimeUs : nowUs;
            const double heardFrame = m_startFrame + double(audioPlayedFrames) * m_fps / kSampleRate
                + double(std::max<int64_t>(0, nowUs - sampleUs)) * framesPerUs;
            if (!m_audioStarted) {
                m_audioStarted = true;
                m_baseFrame = heardFrame;
                m_baseUs = nowUs;
            }
            const double predicted = m_baseFrame + double(nowUs - m_baseUs) * framesPerUs;
            const double error = heardFrame - predicted;
            m_lastError = error;
            // Slew once per report (not per call), so the correction rate does not depend on
            // how often the engine polls and repeated stale reports are not counted twice.
            // Device positions are lower bounds of what is heard (quantised to periods, measured
            // a little before they are read), so the clock follows their upper envelope: a
            // report ahead of the clock pulls it quickly, one behind only slowly.
            const double gain = error > 0 ? kSlewGainAhead : kSlewGainBehind;
            m_baseFrame = std::abs(error) > kSnapFrames ? heardFrame : predicted + error * gain;
            m_baseUs = nowUs;
        }
        const double pos = m_baseFrame + double(nowUs - m_baseUs) * framesPerUs;
        m_last = std::max(m_last, pos); // forward-only
        return m_last;
    }
    const double pos = m_baseFrame + m_rate * double(nowUs - m_baseUs) * framesPerUs;
    m_last = m_rate >= 0 ? std::max(m_last, pos) : std::min(m_last, pos);
    return m_last;
}

} // namespace kk
