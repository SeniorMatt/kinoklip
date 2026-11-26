// SPDX-License-Identifier: GPL-3.0-or-later
// LutPaths — LUT files named by a project (a grade node's LUT, a clip's input LUT) are stored
// twice in the project file: the absolute path, and next to it the path relative to the project
// file ("lutRelative", "inputLutRelative"). A project moved together with its LUT folder finds
// the files again, as the media pool does with "relativePath" and effect File parameters do
// (core/FileParams.h). Works on the project document, so it covers every place a LUT can be.
#pragma once

#include <QDir>
#include <QJsonObject>

namespace kk::lutpaths {

/// @p root with a "<key>Relative" next to every non-empty absolute "lut" / "inputLut" string
/// (relative to @p projectDir; "../Luts/x.cube" when the LUT lies beside the project).
[[nodiscard]] QJsonObject addRelative(const QJsonObject& root, const QDir& projectDir);

/// @p root with every "lut" / "inputLut" made absolute: the stored path if the file exists, else
/// the "<key>Relative" path resolved against @p projectDir if that file exists, else the stored
/// path unchanged. A stored path that is not absolute is resolved against @p projectDir.
[[nodiscard]] QJsonObject resolve(const QJsonObject& root, const QDir& projectDir);

} // namespace kk::lutpaths
