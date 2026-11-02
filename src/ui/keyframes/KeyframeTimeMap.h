// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeTimeMap — the horizontal mapping (timeline frames <-> widget x) shared by every
// keyframe view: the inline timeline lanes map through TimelineLayout, the Keyframes panel
// has its own zoom and scroll.
#pragma once

namespace kk {

/// Maps timeline frames to x and back for keyframe views (dope sheet, curves, ruler).
///
/// Every view that uses one map shares the x origin, so a ruler, a dope sheet and a curve view
/// stacked in a column line up. Implementations are cheap value lookups (called per key per
/// paint). Thread-safety: GUI thread only.
class KeyframeTimeMap
{
public:
    virtual ~KeyframeTimeMap() = default;
    /// Widget x of (fractional) timeline frame @p frame.
    [[nodiscard]] virtual double frameToX(double frame) const = 0;
    /// Fractional timeline frame at widget x @p x.
    [[nodiscard]] virtual double xToFrame(double x) const = 0;
    /// Pixels per timeline frame (> 0).
    [[nodiscard]] virtual double pixelsPerFrame() const = 0;
};

/// A plain linear map: x = originX + (frame - firstFrame) * pixelsPerFrame. Used by the
/// Keyframes panel (its own zoom / scroll) and by tests.
class LinearTimeMap : public KeyframeTimeMap
{
public:
    [[nodiscard]] double frameToX(double frame) const override
    {
        return originX + (frame - firstFrame) * ppf;
    }
    [[nodiscard]] double xToFrame(double x) const override
    {
        return firstFrame + (x - originX) / ppf;
    }
    [[nodiscard]] double pixelsPerFrame() const override { return ppf; }

    double originX = 0.0; ///< x of firstFrame
    double firstFrame = 0.0; ///< timeline frame at originX (fractional)
    double ppf = 4.0; ///< pixels per frame
};

} // namespace kk
