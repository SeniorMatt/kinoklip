// SPDX-License-Identifier: GPL-3.0-or-later
// ClipGeometry — the size of a clip's picture ("content") in timeline pixels, as the compositor
// lays it out, for UI that works in clip content coordinates (masks: Inspector, viewer overlay).
#pragma once

#include "core/TimelineData.h"

#include <QSizeF>

namespace kk {

class MediaPool;

/// Content size of @p clip in timeline pixels at zoom 1: a media clip's display size (SAR and
/// rotation applied) fitted into the @p frameSize, titles and solids cover the frame. Media
/// without a known size (offline) also use @p frameSize. Same rule as render::fitSize().
[[nodiscard]] QSizeF clipContentSize(const MediaPool* pool, const ClipData& clip, QSizeF frameSize);

} // namespace kk
