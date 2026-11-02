// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorGlyphs — small vector glyphs (reset, remove, move up/down, add, expand) drawn with
// QPainter for the inspector's compact tool buttons.
#pragma once

#include <QIcon>

namespace kk {

/// The glyphs used by inspector tool buttons.
enum class InspectorGlyph {
    Reset, ///< anticlockwise circular arrow
    Remove, ///< a cross
    MoveUp, ///< chevron pointing up
    MoveDown, ///< chevron pointing down
    Add, ///< a plus
    Expanded, ///< chevron pointing down (group open)
    Collapsed ///< chevron pointing right (group closed)
};

/// Icon for @p glyph tinted with the application palette's WindowText colour (disabled mode
/// dimmed). Painted from the palette at paint time, so they follow colour scheme switches.
[[nodiscard]] QIcon inspectorGlyph(InspectorGlyph glyph);

} // namespace kk
