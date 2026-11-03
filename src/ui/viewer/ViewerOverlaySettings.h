// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerOverlaySettings — the user's viewer overlay options shared by every viewer: guides
// shown / locked / snapping and their colour, rulers, safe areas, aspect marker and grid.
// Persisted per user in QSettings ("viewer/overlays/..."), bound to the View menu actions.
#pragma once

#include "core/ViewerGuides.h"

#include <QColor>
#include <QObject>
#include <QPointer>

#include <vector>

class QMenu;

namespace kk {

class ActionRegistry;
class GuideOverlay;

/// Display options of the viewer overlays (spec 41). The guide *positions* are document data
/// (TimelineData::guides, saved with the project); these options are how the user likes to
/// see them, so they are per user and the same in every viewer and project.
struct ViewerOverlayOptions
{
    bool showGuides = true; ///< View > Guides > Show Guides
    bool lockGuides = false; ///< View > Guides > Lock Guides (no mouse edits)
    bool snapToGuides = true; ///< View > Guides > Snap to Guides (on-screen transform drags)
    QColor guideColor; ///< invalid = the scheme's default guide colour
    bool showRulers = false; ///< View > Show Rulers
    bool actionSafe = false; ///< View > Safe Area > Action Safe (90 %)
    bool titleSafe = false; ///< View > Safe Area > Title Safe (80 %)
    bool centreMark = false; ///< View > Safe Area > Centre Mark
    double aspectMarker = 0.0; ///< width / height of the marker (2.39, 1, 0.5625...); 0 = off
    bool showGrid = false; ///< View > Grid > Show Grid
    int gridDivisions = 3; ///< 3 = rule of thirds; 2..guides::kMaxGridDivisions

    friend bool operator==(const ViewerOverlayOptions&, const ViewerOverlayOptions&) = default;
};

/// Process-wide holder of ViewerOverlayOptions, like PlaybackResolution.
///
/// - options() / setOptions() read and store the options (QSettings) and emit changed();
///   every GuideOverlay and ViewerRuler repaints on it.
/// - connectActions() binds the View menu commands: the checkable ones follow the options
///   both ways; the guide commands (add, clear, colour, presets) act on the guide overlay of
///   the target viewer (targetOverlay()).
///
/// Thread-safety: GUI thread only.
class ViewerOverlaySettings : public QObject
{
    Q_OBJECT
public:
    /// The process-wide instance (created on first use, loads the stored options).
    static ViewerOverlaySettings& instance();

    [[nodiscard]] const ViewerOverlayOptions& options() const { return m_options; }
    /// Stores @p options and emits changed() if they differ.
    void setOptions(const ViewerOverlayOptions& options);
    /// Re-reads the stored options (tests reset QSettings); emits changed() if needed.
    void load();
    /// The guide colour to paint: the chosen one, else the scheme default.
    [[nodiscard]] QColor guideColor() const;

    /// Binds the "view.guides_*", "view.guide_*", "view.show_rulers", "view.safe_*",
    /// "view.aspect_*" and "view.grid_*" actions of @p actions. Safe to call once per
    /// registry (a later registry replaces the earlier one); actions missing in @p actions
    /// are skipped.
    void connectActions(ActionRegistry* actions);

    /// The overlay the guide commands act on: the active viewer's if it is visible, else the
    /// first visible timeline viewer's, else the first viewer's; nullptr without viewers.
    [[nodiscard]] static GuideOverlay* targetOverlay();

    /// Aspect ratios offered in View > Safe Area (width / height), with their action ids.
    struct AspectChoice
    {
        const char* actionId;
        double aspect;
    };
    [[nodiscard]] static const std::vector<AspectChoice>& aspectChoices();

    /// File of the user's guide presets (default guides::userPresetPath()).
    [[nodiscard]] QString presetPath() const;
    /// Overrides presetPath() (tests); empty = the default.
    void setPresetPath(const QString& path) { m_presetPath = path; }
    /// Adds @p preset to the user presets, replacing one of the same name.
    bool saveUserPreset(const GuidePreset& preset, QString* error = nullptr);
    /// Removes the user preset named @p name.
    bool deleteUserPreset(const QString& name, QString* error = nullptr);
    /// Fills View > Guides > Load Guide Preset (@p load: built-in and user presets, applied
    /// to targetOverlay()) and Delete Guide Preset (@p remove: user presets) each time they
    /// open. Either may be nullptr.
    void attachPresetMenus(QMenu* load, QMenu* remove);

Q_SIGNALS:
    void changed();

private:
    ViewerOverlaySettings();
    void syncActions();

    ViewerOverlayOptions m_options;
    QPointer<ActionRegistry> m_actions; ///< the registry bound last (tests make several)
    QString m_presetPath; ///< setPresetPath()
    bool m_syncing = false; ///< syncActions() is writing check states (no feedback loop)
};

} // namespace kk
