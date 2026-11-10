// SPDX-License-Identifier: GPL-3.0-or-later
// RenderFrameCommand — `kinoklip --render-frame <project.kkproj> <frame> <out.png>`: renders one
// timeline frame of a project file to an image (visual checks, scripting, bug reports).
#pragma once

#include "render/RenderTypes.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <cstdint>

namespace kk {

class Project;

/// Renders timeline frame @p frame of the active timeline of @p project at @p outSize (empty =
/// timeline resolution), the way --render-frame does. A null image (and @p error) on failure.
/// Needs a QGuiApplication; falls back to the CPU renderer without OpenGL.
[[nodiscard]] QImage renderFrameImage(Project& project, int64_t frame, QSize outSize,
    QString* error, Quality quality = Quality::Full);

/// Loads @p projectPath, renders timeline frame @p frame at @p outSize (empty = timeline
/// resolution) and writes it to @p outPath (format from the suffix; PNG keeps alpha).
/// Needs a QGuiApplication (GPU path; falls back to the CPU renderer without OpenGL).
/// Prints a one-line summary to stdout and errors to stderr.
/// @return process exit code: 0 ok, 1 failure.
int runRenderFrame(const QString& projectPath, int64_t frame, const QString& outPath,
    QSize outSize = {}, Quality quality = Quality::Full);

} // namespace kk
