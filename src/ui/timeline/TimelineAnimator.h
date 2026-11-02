// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineAnimator — the display-only tweens of a timeline view (v2.2): clips sliding to their
// new place after an edit, appearing / fading out, the blade flash, trim edge flashes, the
// edit point pulse, the snap indicator fade and playhead glides.
#pragma once

#include "core/TimelineData.h"
#include "ui/common/UiAnimation.h"

#include <QHash>
#include <QRectF>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace kk {

class TimelineLayout;

/// Animates how a TimelineWidget *shows* the model, never the model itself.
///
/// The timeline data and the undo stack change instantly; after every committed change the
/// widget hands the new data to modelChanged(), which compares it with the previous state by
/// clip id and records, for each clip that moved, where it was *displayed* (so a new edit
/// during an animation restarts from what is on screen). The painter then asks displayClip()
/// for the interpolated rect of each clip. Live changes during a drag (an open edit) are not
/// animated: the widget calls markStale() and resync() once the gesture is over, so drags stay
/// 1:1 with the pointer. Hit testing never looks here: it uses the model.
///
/// Time comes from uianim::nowMs() (a test clock can be installed); tick() is called once per
/// paint and freezes "now" for that paint. While anything runs, a precise timer requests
/// repaints of the view at about the display refresh rate; when nothing runs, nothing extra
/// happens (and a disabled animator does no work at all: the widget skips every call).
///
/// Thread-safety: GUI thread only.
class TimelineAnimator
{
public:
    /// @p repaint is called on every animation frame (the widget updates its viewport).
    explicit TimelineAnimator(std::function<void()> repaint);

    // ---- Model changes ----

    /// A committed change: @p now is the new state of timeline @p uid at revision
    /// @p revision, @p oldLayout the view geometry *before* it is rebuilt for @p now. Starts
    /// the tweens of moved, new and removed clips (and the blade flash for clips that were
    /// split). Falls back to resync() when there is no valid previous state, the timeline is
    /// another one, or the change is a mass change (a load).
    void modelChanged(std::shared_ptr<const TimelineData> now, quint64 uid, quint64 revision,
        const TimelineLayout& oldLayout);
    /// Takes @p now as the previous state without animating (and stops the clip tweens).
    void resync(std::shared_ptr<const TimelineData> now, quint64 uid, quint64 revision);
    /// The model changed without a snapshot (a live drag): the next change resyncs.
    void markStale() { m_stale = true; }
    [[nodiscard]] bool stale() const { return m_stale; }
    /// Revision of the state last seen by modelChanged() / resync().
    [[nodiscard]] quint64 revision() const { return m_revision; }
    /// Stops everything and forgets the previous state (animations turned off, project load).
    void clear();
    /// Ends every running animation at once (the next paint shows the final state).
    void finishAll();

    // ---- Other effects ----

    /// A short bright line across the clips of @p track at @p frame (trim release).
    void flashEdge(TrackRef track, int64_t frame);
    /// Pulses the selected edit point highlight once.
    void pulseEditPoint();
    /// The playhead moved from @p fromFrame to @p toFrame: glides the drawn line (see
    /// uianim::PlayheadGlide::moved() for when).
    void playheadMoved(int64_t fromFrame, int64_t toFrame, bool allowed, double pixelsPerFrame);
    /// Called before each paint with the snap indicator the view wants: a new indicator fades
    /// in, a removed one fades out.
    void observeSnap(std::optional<int64_t> frame);
    /// Called before each paint with the selected edit point: a new selection pulses once.
    void observeEditPoint(quint64 tailClip, quint64 headClip);

    /// Freezes "now" for the coming paint, drops finished animations and starts / stops the
    /// repaint timer. Returns whether anything is animating (else the painter can ignore this
    /// object).
    bool tick();
    /// True if something is still animating (as of the last tick()).
    [[nodiscard]] bool active() const { return m_active; }

    // ---- Painter queries (valid after tick()) ----

    /// Turns @p rect, the model rect of clip @p clipId (TimelineLayout::clipRect()), into the
    /// rect to draw; returns the opacity (1 = opaque). Ghosts of removed clips are found by id
    /// too. Untouched clips keep @p rect and return 1.
    double displayClip(quint64 clipId, QRectF& rect, const TimelineLayout& layout) const;
    /// True if clip @p clipId is tweening (moving, appearing or separating).
    [[nodiscard]] bool isAnimating(quint64 clipId) const { return m_clips.contains(clipId); }
    /// Largest horizontal distance (frames) between a clip's drawn and model position; the
    /// painter widens its visible clip range by it.
    [[nodiscard]] double maxShiftFrames() const { return m_clips.isEmpty() ? 0.0 : m_maxShift; }

    /// A removed clip fading out (drawn by the painter like a clip).
    struct Ghost
    {
        ClipData clip;
        TrackRef track; ///< the track it was on (drawn under that row's clips)
        double start = 0, end = 0; ///< frames as displayed when it was removed
        double top = 0, height = 0; ///< view y + scrollY, height
    };
    [[nodiscard]] const std::vector<Ghost>& ghosts() const { return m_ghosts; }

    /// A blade / trim flash in drawing terms.
    struct FlashView
    {
        TrackRef track;
        int64_t frame = 0;
        double wipe = 1; ///< fraction of the clip height the line covers (top down)
        double strength = 0; ///< 1 = full brightness .. 0 = gone
    };
    /// The flashes to draw now (rebuilt by tick(); empty when none).
    [[nodiscard]] const std::vector<FlashView>& flashes() const { return m_flashViews; }
    /// Edit point pulse, 0 (none) .. 1 (peak).
    [[nodiscard]] double editPulse() const;
    /// Fractional playhead frame to draw for model playhead @p modelFrame.
    [[nodiscard]] double playheadFrame(int64_t modelFrame) const;
    /// Snap indicator to draw: frame and opacity (0..1); nullopt = none. @p wanted is the
    /// indicator the view wants now (shown at full opacity unless fading in).
    [[nodiscard]] std::optional<std::pair<int64_t, double>> snapLine(
        std::optional<int64_t> wanted) const;

private:
    /// Where a clip was drawn: frames horizontally, view y + scrollY vertically.
    struct Geom
    {
        double start = 0, end = 0, top = 0, height = 0;
    };
    enum Flag : uint8_t {
        HasFrom = 1, ///< tween from @c from to the model rect
        Appear = 2, ///< fade / grow in
        SplitLeft = 4, ///< left half of a blade cut: right edge pulls back, then settles
        SplitRight = 8 ///< right half: left edge pushed right, then settles
    };
    struct ClipAnim
    {
        Geom from;
        uint8_t flags = 0;
    };
    /// Where clip @p id of the previous state sits in m_prev.
    struct Where
    {
        TrackRef track;
        int index = 0;
    };
    struct Flash
    {
        TrackRef track;
        int64_t frame = 0;
        int64_t startMs = 0;
    };

    /// Clip tween progress (0..1) at @p nowMs (1 when none runs).
    [[nodiscard]] double clipProgress(int64_t nowMs) const;
    /// Geometry of clip @p clip on @p track as drawn by @p layout at the clip progress @p t
    /// (its tween applied if it has one), or nullopt when the track has no row.
    [[nodiscard]] std::optional<Geom> displayedGeom(
        const ClipData& clip, TrackRef track, const TimelineLayout& layout, double t) const;
    void index(const TimelineData& data);
    void ensureTimer();

    std::function<void()> m_repaint;
    QTimer m_timer;

    std::shared_ptr<const TimelineData> m_prev; ///< state of the last modelChanged()/resync()
    QHash<quint64, Where> m_where; ///< clip id -> place in m_prev
    quint64 m_uid = 0;
    quint64 m_revision = 0;
    bool m_stale = true;

    QHash<quint64, ClipAnim> m_clips;
    std::vector<Ghost> m_ghosts;
    int64_t m_clipStartMs = 0;
    double m_maxShift = 0;

    std::vector<Flash> m_flashes;
    std::vector<FlashView> m_flashViews;
    int64_t m_pulseStartMs = -1;
    quint64 m_editTail = 0, m_editHead = 0;

    std::optional<int64_t> m_snap; ///< indicator the view wanted at the last paint
    int64_t m_snapInMs = -1; ///< fade-in start (-1 = shown fully)
    std::optional<int64_t> m_snapOut; ///< indicator fading out
    int64_t m_snapOutMs = 0;

    uianim::PlayheadGlide m_glide;

    int64_t m_now = 0; ///< "now" of the current paint (tick())
    bool m_active = false;
};

} // namespace kk
