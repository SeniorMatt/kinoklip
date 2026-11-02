// SPDX-License-Identifier: GPL-3.0-or-later
// EditBehaviour — editing conventions that differ between editors (durations, what Delete does
// without a selection, three-point editing, trim zones), read from QSettings.
#pragma once

#include "core/Rational.h"

#include <cstdint>

namespace kk {

/// Convention-dependent editing behaviour (plan.md 5.9 rule 5). The defaults follow the
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
    /// Trim start / end to playhead (Shift+[ / Shift+]) ripple the following clips (spec 8.10).
    bool trimToPlayheadRipples = true;
    /// Selection tool: the exact centre of a cut between two touching clips rolls the edit
    /// (spec 8.2); off: the selection tool only trims one side.
    bool selectToolRollsAtCut = true;
    /// Trim tool: the upper (name bar) part of a clip body slides, the rest slips (spec 8.6).
    bool trimToolSlideInNameBar = true;

    /// Values stored in QSettings (group "edit/": stillSeconds, generatorSeconds,
    /// transitionSeconds, multiFrameNudge, threePointEditing, rippleDeleteGapAtPlayhead,
    /// trimToPlayheadRipples, selectToolRollsAtCut, trimToolSlideInNameBar); defaults where
    /// unset.
    [[nodiscard]] static EditBehaviour fromSettings();

    /// @p seconds as a frame count at @p fps (at least 1 frame).
    [[nodiscard]] static int64_t frames(double seconds, Rational fps);
};

} // namespace kk
