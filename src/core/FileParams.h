// SPDX-License-Identifier: GPL-3.0-or-later
// FileParams — the project-file form of File parameters (ParamType::File, e.g. a LUT of the
// Apply LUT effect). In memory a File parameter is an absolute path (QString static); in the
// project file it is {"file": absolute, "relative": path relative to the project}, so a
// project moved together with its LUT folder still finds the files (like the media pool's
// relativePath). Used by Project::toJson() / fromJson().
#pragma once

#include "core/TimelineData.h"

#include <QDir>

namespace kk::fileparams {

/// JSON keys of the stored form (a QVariantMap static while encoded).
inline constexpr char kFileKey[] = "file";
inline constexpr char kRelativeKey[] = "relative";

/// True if an effect of @p timeline has a File parameter with a non-empty path (a cheap scan
/// so saving copies only timelines that need encoding).
[[nodiscard]] bool hasFileParams(const TimelineData& timeline);

/// Replaces every non-empty File parameter path by the stored form relative to
/// @p projectDir (a QVariantMap {file, relative}).
void encodeForSave(TimelineData& timeline, const QDir& projectDir);

/// Turns stored File parameters back into absolute paths: the absolute path if it exists,
/// else the relative path resolved against @p projectDir if that exists, else the absolute
/// path unchanged (the effect then reports the missing file). A plain relative string is
/// resolved against @p projectDir as well.
void decodeAfterLoad(TimelineData& timeline, const QDir& projectDir);

} // namespace kk::fileparams
