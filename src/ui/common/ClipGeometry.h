// SPDX-License-Identifier: GPL-3.0-or-later
// ClipGeometry — the size of a clip's picture ("content") in timeline pixels, as the compositor
// lays it out, for UI that works in clip content coordinates (masks: Inspector, viewer overlay).
#pragma once

#include "core/TimelineData.h"

#include <QSizeF>

namespace kk {

class Project;

/// Content size of @p clip in timeline pixels at zoom 1: a media clip's display size (SAR and
/// rotation applied) or a nested clip's timeline resolution fitted into @p frameSize; titles
/// and solids cover the frame. Unknown sizes (offline media, missing timeline) also use
/// @p frameSize. Same rule as the compositor (render::fitSize()).
[[nodiscard]] QSizeF clipContentSize(
    const Project* project, const ClipData& clip, QSizeF frameSize);

} // namespace kk
