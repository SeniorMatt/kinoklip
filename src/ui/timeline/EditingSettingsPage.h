// SPDX-License-Identifier: GPL-3.0-or-later
// EditingSettingsPage — the "Editing" preferences page owned by the timeline module (mouse
// wheel convention, zoom anchor); registered into the application's SettingsPageRegistry.
#pragma once

namespace kk {

class SettingsPageRegistry;

/// Registers the "editing" page (EditBehaviour: "Zoom around: Playhead / Mouse cursor" and
/// the mouse wheel scheme). The page reads and writes QSettings "edit/*" itself.
void registerEditingSettingsPage(SettingsPageRegistry& registry);

} // namespace kk
