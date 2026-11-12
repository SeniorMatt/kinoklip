// SPDX-License-Identifier: GPL-3.0-or-later
// VimWindowNav — moving the keyboard focus between the panels of the main window (spec section
// 83): <C-h> <C-j> <C-k> <C-l> and <C-w> h j k l go to the nearest visible panel in that direction
// by its place on the screen; <C-w> w / W step through the panels in reading order (top left to
// bottom right, wrapping round, as Neovim's windows.txt does); <leader>w{x} focuses a panel by
// its key. Pure widget functions: no state.
#pragma once

#include <QString>
#include <QVector>

class QWidget;

namespace kk {

class PanelFrame;

namespace vimwin {

enum class Dir {
    Left,
    Down,
    Up,
    Right
};

/// The panels shown in @p window now (visible, not floating in a window of their own), in
/// reading order: top to bottom, then left to right.
[[nodiscard]] QVector<PanelFrame*> visiblePanels(QWidget* window);
/// The panel @p w is in, or nullptr.
[[nodiscard]] PanelFrame* frameOf(QWidget* w);
/// The nearest panel of @p panels in direction @p dir from @p from: the one whose edge is
/// closest in that direction among those that overlap @p from across it (the bigger overlap
/// wins a tie); if none overlaps, the nearest by the centres. nullptr when there is nothing
/// that way.
[[nodiscard]] PanelFrame* neighbour(PanelFrame* from, Dir dir, const QVector<PanelFrame*>& panels);
/// The panel @p delta steps after @p from in reading order, wrapping round (negative: before).
[[nodiscard]] PanelFrame* step(PanelFrame* from, int delta, const QVector<PanelFrame*>& panels);
/// Gives the keyboard focus to @p frame: its timeline / viewer when it has one, else its
/// biggest list or tree, else the panel itself. Returns whether something took the focus.
bool focusPanel(PanelFrame* frame);
/// The list or tree a panel's keys work on (the biggest visible, focusable item view), or
/// nullptr.
[[nodiscard]] QWidget* mainViewOf(PanelFrame* frame);

} // namespace vimwin

} // namespace kk
