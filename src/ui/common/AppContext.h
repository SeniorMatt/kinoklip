// SPDX-License-Identifier: GPL-3.0-or-later
// AppContext — the shared objects handed to every page and panel (dependency injection).
#pragma once

#include <QString>

#include <functional>

namespace kk {

class ActionRegistry;
class PlaybackEngine; // src/playback; null in tests that do not need playback
class MediaManagement; // proxies / optimized media / render cache; may be null in tests
class MediaStore; // single-file projects copy imports in; may be null (= linked)
class PowerBins; // bins shared by every project; may be null in tests
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
    MediaManagement* media = nullptr; ///< proxies, optimized media, render cache; may be null
    PowerBins* powerBins = nullptr; ///< power bins (core/PowerBins.h); null = none shown
    /// Where imported media go (ui/common/MediaStore.h); null = used where they are (linked).
    MediaStore* mediaStore = nullptr;
    /// The project bin shown in the media pool ("" = Master), set by the shown MediaPoolPanel;
    /// imports without a bin of their own go there (importTargetBin(), ui/common/MediaImport.h).
    /// Null = Master.
    std::function<QString()> currentMediaPoolBin;
};

} // namespace kk
