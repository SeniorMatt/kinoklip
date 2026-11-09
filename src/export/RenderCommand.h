// SPDX-License-Identifier: GPL-3.0-or-later
// RenderCommand — `kinoklip --render <project.kkproj> <out-file> [--preset id]`: renders a
// project file from the command line (scripting, CI, end-to-end tests).
#pragma once

#include "export/ExportJob.h"
#include "export/ExportSettings.h"

#include <QString>

namespace kk {

/// Default settings for an output file name when no preset is given: chosen by extension
/// (mp4 → H.264/AAC at timeline size, mov → ProRes 422 HQ, mkv → H.264 + FLAC, webm → VP9 +
/// Opus, wav/flac/mp3/m4a/opus → audio only, gif → GIF, png/jpg → single frame, a "%d"
/// pattern with png/jpg/tif/exr → image sequence). @return false for unknown extensions.
[[nodiscard]] bool settingsForOutputFile(
    const QString& outPath, ExportSettings& settings, QString* error);

class Project;

/// Outcome of renderProject().
struct RenderOutcome
{
    bool ok = false;
    QString error; ///< why not (unknown preset, export error)
    ExportResult result;
};

/// Renders the active timeline of @p project (whole timeline, or in/out if both marks are set)
/// to @p outPath with preset @p presetId (empty = settingsForOutputFile()), reporting progress
/// to @p progress (may be empty). Needs a QGuiApplication. runRender() and the `render`
/// command (app/commands) are built on it.
[[nodiscard]] RenderOutcome renderProject(Project& project, const QString& outPath,
    const QString& presetId, const ExportJob::ProgressCallback& progress);

/// Loads @p projectPath and renders its timeline (whole timeline, or in/out if both marks
/// are set) to @p outPath with preset @p presetId (empty = settingsForOutputFile()).
/// Needs a QGuiApplication. Prints progress to stdout and errors to stderr.
/// @return process exit code: 0 ok, 1 failure.
int runRender(const QString& projectPath, const QString& outPath, const QString& presetId = {});

} // namespace kk
