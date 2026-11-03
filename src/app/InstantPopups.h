// SPDX-License-Identifier: GPL-3.0-or-later
// InstantPopups — menus, context menus, combo-box lists and other popups open and close at
// once, like Resolve's (Reddit feedback on 1.0.0-beta.1: fading menus slowed the work down).
#pragma once

class QApplication;
class QWidget;

namespace kk::instantpopups {

/// Turns off every popup animation the application controls and keeps it off:
/// - Qt's own UI effects (Qt::UI_AnimateMenu, UI_FadeMenu, UI_AnimateCombo, UI_AnimateTooltip,
///   UI_FadeTooltip). The platform theme turns them on from the desktop settings: on Windows
///   from the system's menu / tooltip animation settings, on KDE partly from kdeglobals.
///   UI_General stays as the desktop set it (it only gates the effects above here).
/// - The menu style hints that delay or fade a closing menu (SH_Menu_FadeOutOnHide,
///   SH_Menu_FlashTriggeredItem): see MenuMetricsStyle.
/// - Compositor animations, as far as a window can opt out (markWindow()).
/// Idempotent; called by theme::apply(). The non-menu UI animations (uianim, timeline edits,
/// playhead glides) are not affected. GUI thread only.
void install(QApplication& app);

/// True if none of the popup effects above is enabled (tests).
[[nodiscard]] bool effectsOff();

/// Asks the compositor not to animate the popup window @p popup (called for every shown
/// Qt::Popup / Qt::ToolTip window once install() ran):
/// - X11 (KWin): sets _KDE_NET_WM_SKIP_CLOSE_ANIMATION, which KWin's "Fading popups" effect
///   honours for the closing fade. The opening fade of that effect has no per-window opt-out.
/// - Windows: DWMWA_TRANSITIONS_FORCEDISABLED (no DWM show / hide transition).
/// - Wayland: no protocol lets a client opt out; KWin's "Fading popups" (System Settings >
///   Desktop Effects) still fades xdg popups there.
/// Returns true if a hint was set. Safe on any platform (no-op on offscreen / Wayland).
bool markWindow(QWidget* popup);

} // namespace kk::instantpopups
