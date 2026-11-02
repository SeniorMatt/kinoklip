// SPDX-License-Identifier: GPL-3.0-or-later
// MultiClipEdit — Inspector edits applied to every selected clip of the same kind (spec 24.1):
// which clips are peers, how a change of the shown clip is copied onto them, and whether a
// parameter has mixed values across them.
#pragma once

#include "ui/inspector/ParamAccess.h"

#include <QVector>

#include <optional>

namespace kk {

struct TimelineData;

namespace multiclip {

/// The selected clips that an edit of clip @p shownId also changes: same ClipKind (and
/// generator type), on a track of the same type (video members for the Video tab, audio
/// members for the Audio tab), not on a locked track; @p shownId itself is not included.
/// Order follows @p selection. Empty when @p shownId is not selected (e.g. a track style).
[[nodiscard]] QVector<quint64> peers(
    const TimelineData& data, const QVector<quint64>& selection, quint64 shownId);

/// The parameter of @p peer that corresponds to @p ref on @p shown: the same scope and key;
/// effect parameters match the effect instance of the same type at the same position among
/// the clip's instances of that type (the second blur matches the second blur). Masks and
/// transitions are per clip: std::nullopt (as when the peer has no matching effect).
[[nodiscard]] std::optional<ParamRef> peerRef(
    const ClipData& shown, const ClipData& peer, const ParamRef& ref);

/// Copies every parameter that differs between @p before and @p after (the shown clip just
/// before and after an Inspector edit) onto @p peer: whole parameters (value and keys, or the
/// static), in the transform, audio, title and solid colour sets and the parameters and
/// enabled state of matching effect instances. Structure edits (adding, removing or
/// reordering effects or masks, transitions) are not copied. Returns whether @p peer changed.
bool propagate(const ClipData& before, const ClipData& after, ClipData& peer);

} // namespace multiclip
} // namespace kk
