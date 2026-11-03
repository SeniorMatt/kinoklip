// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerGuides — guide lines placed on the viewer picture (never rendered): the per-timeline
// data, its JSON form, guide presets (built-in and the user's) and the geometry of the
// safe-area, aspect-marker and grid overlays.
#pragma once

#include <QJsonArray>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <optional>
#include <span>
#include <vector>

namespace kk {

/// One guide line on the viewer picture.
///
/// A Horizontal guide is a line across the picture at y = position; a Vertical one is a line
/// from top to bottom at x = position. Positions are timeline frame pixels from the top / left
/// edge of the picture (the unit the rulers show), not widget pixels, so a guide stays on the
/// same picture point at any viewer zoom or size. They may be fractional (a dialog entry in
/// percent) and may lie outside the frame only transiently while being dragged.
struct ViewerGuide
{
    Qt::Orientation orientation = Qt::Horizontal;
    double position = 0.0; ///< timeline frame px from the top (Horizontal) / left (Vertical)

    friend bool operator==(const ViewerGuide&, const ViewerGuide&) = default;
};

/// A named set of guides in fractions of the frame (0 = top / left, 1 = bottom / right), so a
/// preset fits every timeline resolution.
struct GuidePreset
{
    QString id; ///< "builtin:centre", "builtin:thirds", or "user:<name>"
    QString name; ///< shown in View > Guides > Load Guide Preset
    bool builtIn = false;
    std::vector<ViewerGuide> fractions; ///< position in 0..1 of the frame height / width

    friend bool operator==(const GuidePreset&, const GuidePreset&) = default;
};

namespace guides {

/// Action safe area, fraction of the frame width and height (centred).
inline constexpr double kActionSafe = 0.9;
/// Title safe area, fraction of the frame width and height (centred).
inline constexpr double kTitleSafe = 0.8;
/// Largest grid division count accepted by gridLines() and the Grid Size dialog.
inline constexpr int kMaxGridDivisions = 32;

/// JSON array of @p list: [{"o": "h" | "v", "pos": px}, ...] (project format 14).
[[nodiscard]] QJsonArray toJson(const std::vector<ViewerGuide>& list);
/// Decodes toJson(). Entries with an unknown orientation or a non-finite position are
/// dropped; a missing array (older projects) gives no guides.
[[nodiscard]] std::vector<ViewerGuide> fromJson(const QJsonArray& array);

/// Index of the guide of @p list nearest to the frame point (@p x, @p y) along its own axis
/// within @p tolerance frame px, or -1. Ties go to the guide added last (drawn on top).
[[nodiscard]] int nearestGuide(
    const std::vector<ViewerGuide>& list, double x, double y, double tolerance);

/// The guides of @p preset for a frame of @p frame px.
[[nodiscard]] std::vector<ViewerGuide> presetGuides(const GuidePreset& preset, QSizeF frame);
/// A user preset named @p name holding @p list (frame px of a @p frame frame) as fractions.
[[nodiscard]] GuidePreset makeUserPreset(
    const QString& name, const std::vector<ViewerGuide>& list, QSizeF frame);
/// The built-in presets: Centre Cross, Rule of Thirds, Centre and Thirds.
[[nodiscard]] std::vector<GuidePreset> builtInPresets();

/// Default file of the user presets: <AppDataLocation>/guide-presets.json.
[[nodiscard]] QString userPresetPath();
/// Reads the user presets of @p path (missing or unreadable file: none).
/// Format: {"formatVersion": 1, "presets": [{"name", "guides": [{"o", "pos"}]}]} with "pos"
/// in fractions of the frame.
[[nodiscard]] std::vector<GuidePreset> loadUserPresets(const QString& path = userPresetPath());
/// Writes @p presets to @p path (creating the folder). @return false with @p error on failure.
bool saveUserPresets(const std::vector<GuidePreset>& presets,
    const QString& path = userPresetPath(), QString* error = nullptr);
/// Built-in presets followed by the user presets of @p path.
[[nodiscard]] std::vector<GuidePreset> allPresets(const QString& path = userPresetPath());

/// The centred safe area covering @p fraction of @p frame in each direction (kActionSafe,
/// kTitleSafe), in frame px.
[[nodiscard]] QRectF safeArea(QSizeF frame, double fraction);
/// The largest centred rectangle of aspect @p aspect (width / height, e.g. 2.39, 1.0,
/// 9.0 / 16) inside @p frame, in frame px. An aspect <= 0 gives the whole frame.
[[nodiscard]] QRectF aspectMarker(QSizeF frame, double aspect);
/// Interior grid lines dividing @p frame into @p divisions equal columns and rows (3 = rule
/// of thirds), frame px: @p xs gets the vertical lines, @p ys the horizontal ones. Divisions
/// are clamped to 2..kMaxGridDivisions.
void gridLines(QSizeF frame, int divisions, std::vector<double>& xs, std::vector<double>& ys);

/// Snapping: the offset that moves the nearest of @p features onto the nearest of
/// @p targets, if that distance is at most @p threshold (all in the same unit, frame px).
/// std::nullopt when nothing is in reach. Ties keep the first feature / target.
[[nodiscard]] std::optional<double> snapOffset(
    std::span<const double> features, std::span<const double> targets, double threshold);
/// Positions of the guides of @p list with @p orientation (snap targets of one axis).
[[nodiscard]] std::vector<double> positions(
    const std::vector<ViewerGuide>& list, Qt::Orientation orientation);

/// Ruler tick spacing: the smallest step of 1, 2 or 5 x 10^k frame px whose labelled ticks are
/// at least @p minLabelSpacing widget px apart when one frame px is @p scale widget px.
[[nodiscard]] double rulerStep(double scale, double minLabelSpacing);

} // namespace guides

} // namespace kk
