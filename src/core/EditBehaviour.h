// SPDX-License-Identifier: GPL-3.0-or-later
// EditBehaviour — editing conventions that differ between editors (durations, what Delete does
// without a selection, three-point editing, trim zones), read from QSettings.
#pragma once

#include "core/Rational.h"

#include <cstdint>

namespace kk {

/// Which point stays fixed when the timeline zooms with the mouse wheel (spec 19.6).
enum class ZoomAnchor {
    Playhead, ///< the playhead keeps its place (Resolve; view centre when it is off-screen)
    Cursor ///< the frame under the mouse pointer keeps its place
};

/// Meaning of the mouse wheel modifiers over the timeline (spec 19.3).
enum class WheelScheme {
    /// Resolve: wheel scrolls the tracks, Ctrl+wheel scrolls time, Alt+wheel zooms time,
    /// Shift+wheel changes the track height
    Resolve,
    /// Kinoklip 0.1: wheel scrolls the tracks (time when they fit), Shift+wheel scrolls time,
    /// Ctrl+wheel zooms
    Classic
};

/// Convention-dependent editing behaviour (docs/archive/plan.md 5.9 rule 5). The defaults follow the
/// Resolve-like profile described in docs/behavior-spec.md; a future interface profile can
/// change them without touching the timeline code. Plain value type; read it where the
/// behaviour happens (cheap: a few QSettings lookups).
struct EditBehaviour
{
    /// Length of stills placed on the timeline (seconds, spec 14.2).
    double stillSeconds = 5.0;
    /// Length of new titles and generators (seconds, spec 14.1).
    double generatorSeconds = 5.0;
    /// Length of the default transition (Ctrl+T, drops; seconds, spec 13.1).
    double transitionSeconds = 1.0;
    /// Frames moved by the "nudge multiple frames" commands (spec 8.9).
    int multiFrameNudge = 5;
    /// Source edits (insert / overwrite / place on top / replace) use the timeline in/out
    /// marks as destination (three-point editing, spec 4.2-4.4). Off: always at the playhead.
    bool threePointEditing = true;
    /// Ripple delete with nothing selected closes the gap under the playhead (a Kinoklip
    /// extension). Off (default, spec 10.5): it does nothing, a gap has to be selected first.
    bool rippleDeleteGapAtPlayhead = false;
    /// Ripple delete with nothing selected (no gap, no in / out range) removes the clips under
    /// the playhead on the auto-select tracks and closes up (spec 49.6). Off by default (spec
    /// 10.5); Resolve's manual documents this for the Speed Editor's ripple delete key.
    bool rippleDeleteClipAtPlayhead = false;
    /// Selection tool: the exact centre of a cut between two touching clips rolls the edit
    /// (spec 8.2); off: the selection tool only trims one side.
    bool selectToolRollsAtCut = true;
    /// Trim tool: the upper (name bar) part of a clip body slides, the rest slips (spec 8.6).
    bool trimToolSlideInNameBar = true;
    /// Fixed point of mouse-wheel zoom ("Timeline zooms around", spec 19.6). Keyboard zoom always
    /// keeps the playhead.
    ZoomAnchor zoomAnchor = ZoomAnchor::Playhead;
    /// Mouse wheel modifier convention (spec 19.3).
    WheelScheme wheelScheme = WheelScheme::Resolve;
    /// Open timelines are shown as tabs above the timeline (owner default, v2); off: one
    /// timeline at a time, chosen from a drop-down like Resolve's single-timeline mode.
    bool timelineTabs = true;
    /// Two-up trim view (spec 8.19): render each side with its clip's effects and transform
    /// through the program compositor instead of showing the source frame. Off by default:
    /// it shares the viewer's render thread and lags behind fast drags (docs/archive/polish-v3, Round 3).
    bool twoUpProgramLook = false;
    /// A press in the timeline ruler away from the playhead's head moves the playhead there at
    /// once and drags it along (Resolve). Off: only a click (released without moving) moves it
    /// there, so a slip of the mouse on the ruler leaves it alone; dragging the head itself
    /// works either way (spec 48.1).
    bool rulerPressJumpsPlayhead = true;

    /// Values stored in QSettings (group "edit/": stillSeconds, generatorSeconds,
    /// transitionSeconds, multiFrameNudge, threePointEditing, rippleDeleteGapAtPlayhead,
    /// rippleDeleteClipAtPlayhead,
    /// selectToolRollsAtCut, trimToolSlideInNameBar, zoomAnchor
    /// ("playhead" / "mouse", shared with prefs::kZoomAnchor), wheelScheme ("resolve" / "classic"),
    /// timelineTabs, twoUpProgramLook, rulerPressJumpsPlayhead); defaults where unset.
    [[nodiscard]] static EditBehaviour fromSettings();

    /// @p seconds as a frame count at @p fps (at least 1 frame).
    [[nodiscard]] static int64_t frames(double seconds, Rational fps);

    /// Stores the zoom anchor (QSettings "edit/zoomAnchor", see prefs::kZoomAnchor).
    static void setZoomAnchor(ZoomAnchor anchor);
    /// Stores the wheel scheme (QSettings "edit/wheelScheme").
    static void setWheelScheme(WheelScheme scheme);

    /// Removes retired keys (called once at start-up): "edit/trimToPlayheadRipples" (until
    /// alpha.2 Shift+[ / Shift+] rippled by default; now Shift+[ / ] leave a gap and
    /// Ctrl+Shift+[ / ] ripple, as in Resolve, so the setting has no meaning any more).
    static void migrateSettings();
};

} // namespace kk
