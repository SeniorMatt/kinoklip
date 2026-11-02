// SPDX-License-Identifier: GPL-3.0-or-later
// AppContext — the shared objects handed to every page and panel (dependency injection).
#pragma once

namespace kk {

class ActionRegistry;
class PlaybackEngine; // src/playback; null in tests that do not need playback
class Project;
class SelectionModel;
class SettingsPageRegistry;

/// Non-owning pointers to the application-wide objects. Created in main(), outlives every
/// widget. Panels take `(AppContext& ctx, QWidget* parent)` and keep the reference.
/// No singletons for model objects: tests can build their own context.
struct AppContext
{
    Project* project = nullptr; ///< the open document (never null in the app)
    SelectionModel* selection = nullptr; ///< selected clips/keyframes (never null in the app)
    ActionRegistry* actions = nullptr; ///< every user command, by id (never null in the app)
    PlaybackEngine* playback = nullptr; ///< program (timeline) playback; may be null in tests
    SettingsPageRegistry* settingsPages = nullptr; ///< preference pages (null: standard pages only)
};

} // namespace kk
