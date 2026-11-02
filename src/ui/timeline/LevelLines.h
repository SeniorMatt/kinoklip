// SPDX-License-Identifier: GPL-3.0-or-later
// LevelLines — geometry and hit-testing of the audio level line (clip volume drawn over the
// waveform, with its keys) and of the fade handles at the top corners of clips. Pure (no
// widgets), shared by TimelinePainter and the input state machine.
#pragma once

#include "core/TimelineData.h"
#include "ui/timeline/TimelineLayout.h"

#include <QPointF>
#include <QRectF>

#include <cstdint>

namespace kk::levellines {

inline constexpr double kNameBand = 15; ///< clip name band height (TimelinePainter)
inline constexpr int kMinBandRow = 30; ///< rows lower than this show only the name band
inline constexpr double kTopDb = 24.0; ///< level at the top of the waveform area (max volume)
inline constexpr double kBottomDb = -60.0; ///< level at the bottom (lower = silence)
inline constexpr double kLineGrab = 4.0; ///< px above / below the line that grab it
inline constexpr double kKeyGrab = 5.0; ///< px around a key that grab it
inline constexpr double kKeyRadius = 3.5; ///< drawn key diamond half size
inline constexpr double kMinAreaHeight = 14.0; ///< lower clips show no level line
inline constexpr double kFadeHandle = 8.0; ///< fade handle square edge (px)
inline constexpr double kMinFadeClipWidth = 30.0; ///< narrower clips show no fade handles
/// Line drag sensitivity with Shift (fine): a tenth of the normal dB per pixel.
inline constexpr double kFineFactor = 0.1;

/// The waveform area of an audio clip whose clip rectangle is @p clipRect (below the name
/// band); empty when the clip is too low for a level line.
[[nodiscard]] QRectF levelArea(const QRectF& clipRect);
/// y of level @p db in @p area (clamped to the area).
[[nodiscard]] double yForDb(const QRectF& area, double db);
/// Level at y in @p area: kTopDb .. kBottomDb; the bottom edge itself means silence (-96 dB).
[[nodiscard]] double dbForY(const QRectF& area, double y);
/// dB per pixel of a line drag in @p area.
[[nodiscard]] double dbPerPixel(const QRectF& area);

/// What a press at a position on an audio clip hits.
struct LevelHit
{
    enum class Kind {
        None,
        Line, ///< the level line (drag = change the volume there; Alt+click = add a key)
        Key ///< a volume key (drag = move it in time and value)
    };
    Kind kind = Kind::None;
    int64_t localFrame = 0; ///< Line: frame under the pointer; Key: the key's frame
};
/// Hit test of @p pos against the level line of @p clip in @p clipRect (keys win).
[[nodiscard]] LevelHit hitLevel(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout, QPointF pos);

/// Fade handle of the head (@p head) or tail of @p clip in @p clipRect: a square at the top
/// edge, at the end of the current fade (the clip corner without one). Empty when the clip is
/// too small for handles.
[[nodiscard]] QRectF fadeHandleRect(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout, bool head);
/// Which fade handle is at @p pos: 1 = head, 2 = tail, 0 = none.
[[nodiscard]] int hitFadeHandle(
    const ClipData& clip, const QRectF& clipRect, const TimelineLayout& layout, QPointF pos);

} // namespace kk::levellines
