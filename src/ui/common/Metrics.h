// SPDX-License-Identifier: GPL-3.0-or-later
// Metrics — the one spacing scale and the chrome sizes shared by every panel, bar and dialog.
#pragma once

namespace kk::metrics {

// ---- Spacing scale (px, logical) ------------------------------------------------------
// Every margin and spacing in our own layouts is one of these; Breeze draws the widgets
// themselves with its own (similar) metrics, so mixing in arbitrary values makes panels
// look misaligned against each other.
inline constexpr int kSpaceXs = 2; ///< between tightly grouped tool buttons
inline constexpr int kSpaceS = 4; ///< inside compact rows (title bars, tool bars)
inline constexpr int kSpaceM = 8; ///< panel body margins, form spacing
inline constexpr int kSpaceL = 12; ///< between groups / sections, dialog margins
inline constexpr int kSpaceXl = 16; ///< page-level gaps, empty state padding

// ---- Chrome sizes ---------------------------------------------------------------------
inline constexpr int kPanelTitleHeight = 30; ///< panel title bars (fits a Breeze combo box)
inline constexpr int kToolIconSize = 16; ///< icons in title bars and the interface bar
inline constexpr int kTransportIconSize = 18; ///< viewer transport / timeline tool bar icons
inline constexpr int kInterfaceBarHeight = 36; ///< top panel-toggle strip
inline constexpr int kPageBarHeight = 42; ///< bottom page strip
/// Minimum width of a combo box in a panel title bar: room for "100%" / "Quarter" plus the
/// Breeze arrow and frame padding (the owner's feedback: Fit / Auto looked cramped).
inline constexpr int kTitleComboMinWidth = 84;
/// Label column of settings forms: wide enough for the longest Resolve-style label so all
/// fields in a dialog start at the same x.
inline constexpr int kFormLabelMinWidth = 150;

} // namespace kk::metrics
