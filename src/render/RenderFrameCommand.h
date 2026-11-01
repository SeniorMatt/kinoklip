// SPDX-License-Identifier: GPL-3.0-or-later
// RenderFrameCommand — `kinoklip --render-frame <project.kkproj> <frame> <out.png>`: renders one
// timeline frame of a project file to an image (visual checks, scripting, bug reports).
#pragma once

#include "render/RenderTypes.h"

#include <QSize>
#include <QString>

#include <cstdint>

namespace kk {

/// Loads @p projectPath, renders timeline frame @p frame at @p outSize (empty = timeline
/// resolution) and writes it to @p outPath (format from the suffix; PNG keeps alpha).
/// Needs a QGuiApplication (GPU path; falls back to the CPU renderer without OpenGL).
/// Prints a one-line summary to stdout and errors to stderr.
/// @return process exit code: 0 ok, 1 failure.
int runRenderFrame(const QString& projectPath, int64_t frame, const QString& outPath,
    QSize outSize = {}, Quality quality = Quality::Full);

} // namespace kk
