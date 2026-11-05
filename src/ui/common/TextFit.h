// SPDX-License-Identifier: GPL-3.0-or-later
// TextFit — text in a part the user cannot resize never runs off its widget (owner rule,
// docs/coding-style.md 5b): such a widget either grows to fit its text or elides it with "..."
// and shows the full text in a tooltip. Never a horizontal scroll bar, never a hard clip.
// Each helper keeps the fit when the font, the style or the text changes (translations,
// larger system fonts). Spec 23.20-23.23 in docs/behavior-spec.md.
#pragma once

#include <QString>

class QLabel;
class QListView;
class QToolButton;

namespace kk::textfit {

/// The width @p list needs to show its widest item (hidden rows included) without eliding
/// or scrolling sideways: item size hints (style padding included), item spacing, frame,
/// contents margins and room for a vertical scroll bar when one can appear.
[[nodiscard]] int listWidthForItems(const QListView* list);

/// Fixes the width of a dialog's section list to max(@p minimumWidth, listWidthForItems())
/// and turns its horizontal scroll bar off. Recomputed when items are added, removed or
/// renamed and on font and style changes.
void fitListWidthToItems(QListView* list, int minimumWidth);

/// For a list whose width is capped by design: items take the viewport width, long texts
/// are elided ("...") with the full text as their tooltip, and there is no horizontal
/// scroll bar. Replaces the list's item delegate.
void elideListItems(QListView* list);

/// Shows @p text in @p label elided per line to the label's width; when anything is elided
/// the tooltip starts with the full text, followed by @p toolTip. Re-elided on resize, font
/// and style changes.
void setElidedText(QLabel* label, const QString& text, const QString& toolTip = {});

/// The same for a tool button's text: elided to the room the button has for text.
void setElidedText(QToolButton* button, const QString& text, const QString& toolTip = {});

} // namespace kk::textfit
