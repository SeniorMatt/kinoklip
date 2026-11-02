// SPDX-License-Identifier: GPL-3.0-or-later
// AutoQuality — the opt-in "Automatic" timeline proxy resolution: decides from measured render
// times whether playback should render at Full or Half. Pure logic (no Qt, no clock of its
// own), driven by PlaybackEngine and unit-tested in test_playbacklogic.
#pragma once

#include "render/RenderTypes.h"

#include <cstdint>

namespace kk {

/// Picks the playback quality for QualityMode::Auto.
///
/// Resolve plays at full resolution and drops frames when it cannot keep up; a lower proxy
/// resolution is the user's choice. Auto is the opt-in middle ground and deliberately
/// conservative:
/// * **Downshift** Full -> Half only after kOverloadWindows consecutive one-second windows in
///   which the renderer itself was the bottleneck (average render time above kDownshiftLoad of
///   the frame interval). Dropped frames alone never downshift: they also come from GUI
///   stalls, seeks or the audio clock, which a smaller picture does not fix.
/// * **Never below Half** (Quarter is only ever an explicit choice).
/// * **Upshift quickly**: after kUpshiftHoldUs of comfortable headroom at Half
///   (load < kUpshiftLoad), or immediately when Half turned out to be hardly faster than Full
///   (decoding, not compositing, was the bottleneck: a smaller picture only costs sharpness).
///   When Full fails again soon after an upshift, the next upshift waits kBackoffUs longer,
///   doubling up to kMaxBackoffUs.
/// * A new play (reset()) starts at Full.
class AutoQuality
{
public:
    static constexpr int64_t kWindowUs = 1'000'000; ///< evaluation window
    static constexpr int64_t kSettleUs = 1'000'000; ///< no evaluation this long after a change
    static constexpr int kOverloadWindows = 3; ///< consecutive overloaded windows to downshift
    static constexpr double kDownshiftLoad = 0.9; ///< render time / frame interval
    static constexpr double kUpshiftLoad = 0.5;
    static constexpr int64_t kUpshiftHoldUs = 2'000'000;
    /// Half must be at least this much faster than Full (render time ratio) to be worth it.
    static constexpr double kUsefulSpeedup = 0.75;
    static constexpr int64_t kBackoffUs = 5'000'000; ///< first extra wait after Full failed again
    static constexpr int64_t kMaxBackoffUs = 30'000'000;

    /// Starts over at Full (playback started) at time @p nowUs.
    void reset(int64_t nowUs);

    /// Feeds one observation: the moving-average render time @p avgRenderMs of the frames
    /// presented recently and the frame interval @p intervalMs, at time @p nowUs. Call it
    /// often (every tick is fine); it only evaluates once per kWindowUs.
    /// @return true if quality() changed (the caller re-renders and restarts its average).
    bool update(int64_t nowUs, double avgRenderMs, double intervalMs);

    [[nodiscard]] Quality quality() const { return m_quality; }
    /// Render load of the last evaluated window (render time / frame interval; 0 = none yet).
    [[nodiscard]] double lastLoad() const { return m_lastLoad; }

private:
    bool change(Quality next, int64_t nowUs);

    Quality m_quality = Quality::Full;
    int64_t m_changedUs = 0;
    int64_t m_windowStartUs = 0;
    int m_overloaded = 0;
    int64_t m_headroomSinceUs = -1;
    double m_fullLoad = 0.0; ///< load measured at Full right before the downshift
    int64_t m_backoffUs = 0; ///< extra wait before upshifting (grows while Full keeps failing)
    int64_t m_upshiftNotBeforeUs = 0;
    int64_t m_upshiftedUs = -1; ///< time of the last upshift
    bool m_stayFull = false; ///< Half did not help: Full for the rest of this play
    double m_lastLoad = 0.0;
};

} // namespace kk
