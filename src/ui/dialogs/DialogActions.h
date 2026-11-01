// SPDX-License-Identifier: GPL-3.0-or-later
// DialogActions — connects the menu commands that open the panels-owned dialogs.
#pragma once

#include <functional>

class QWidget;

namespace kk {

struct AppContext;

/// Connects `file.project_settings`, `edit.keyboard_customization` and `edit.preferences` to
/// their dialogs (modal, parented to @p window) and applies the stored hardware-acceleration
/// preference to the media module. @p onPreferencesChanged runs after the Preferences dialog
/// stored new values (e.g. to restart the autosave timer). Call once from the main window.
void connectDialogActions(
    AppContext& ctx, QWidget* window, std::function<void()> onPreferencesChanged = {});

} // namespace kk
