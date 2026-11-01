// SPDX-License-Identifier: GPL-3.0-or-later
// CommandLine — parsing of the command-line options into a plain struct.
#pragma once

#include <QString>
#include <QStringList>

namespace kk {

/// What the user asked for on the command line.
/// To add a mode (e.g. `--render <project> <out.mp4>`): add fields here, an option in
/// parseCommandLine(), and a branch in main() that runs it before the window is shown.
struct CommandLineOptions
{
    QString projectFile; ///< project to open (optional positional argument)
    QString screenshotPage; ///< --screenshot <page>: "media", "edit" or "deliver"
    QString screenshotPath; ///< PNG written in screenshot mode
    bool resetLayout = false; ///< --reset-layout: ignore saved window/panel layout
    QString renderFrameProject; ///< --render-frame <project>: project to render a frame of
    qint64 renderFrameIndex = 0; ///< timeline frame for --render-frame
    QString renderFramePath; ///< image written by --render-frame

    [[nodiscard]] bool isScreenshot() const { return !screenshotPage.isEmpty(); }
    [[nodiscard]] bool isRenderFrame() const { return !renderFrameProject.isEmpty(); }
};

/// Parses @p arguments (including argv[0]). Handles --help/--version itself (prints and exits).
/// Returns false with a message in @p error for invalid input.
bool parseCommandLine(const QStringList& arguments, CommandLineOptions& options, QString* error);

} // namespace kk
