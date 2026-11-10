// SPDX-License-Identifier: GPL-3.0-or-later
// Command-line parsing with QCommandLineParser.
#include "app/CommandLine.h"

#include <QCommandLineParser>
#include <QCoreApplication>

namespace kk {

bool parseCommandLine(const QStringList& arguments, CommandLineOptions& options, QString* error)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("kk::CommandLine", "Non-linear video editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("project"),
        QCoreApplication::translate("kk::CommandLine", "Project file to open (optional)."),
        QStringLiteral("[project]"));

    const QCommandLineOption screenshot(QStringLiteral("screenshot"),
        QCoreApplication::translate("kk::CommandLine",
            "Render the window (1920x1080) showing <target> to the PNG given as the first "
            "positional argument, then exit. Targets: media, edit, edit-single, edit-dual, "
            "transition, deliver, preferences, "
            "keyboard, inspector, keyframes, keyframes-window, retime, retime:curve, subtitles, "
            "subtitles:inspector, mediapool, effects[:<id>], fx:<id>[:inspector|viewer], "
            "change-speed, timeline, deliver:inout, deliver:queue, import-dialog (whole "
            "screen with File > Import Media open), menu-color-theme, "
            "menu-style, menu-playback-resolution, clip:<track>[:<tab>], "
            "menu:<category>[:<submenu>], normalize-audio. Usage: --screenshot "
            "edit out.png [project]"),
        QStringLiteral("target"));
    const QCommandLineOption resetLayout(QStringLiteral("reset-layout"),
        QCoreApplication::translate("kk::CommandLine", "Ignore the saved window layout."));
    const QCommandLineOption renderFrame(QStringLiteral("render-frame"),
        QCoreApplication::translate("kk::CommandLine",
            "Render one timeline frame of <project> to an image, then exit. Usage: "
            "--render-frame project.kkproj <frame> <out.png>"),
        QStringLiteral("project"));
    // Not "--style": QApplication consumes -style/--style itself before we parse. This one
    // goes through the theme (stored choice untouched, fallback to Breeze/Fusion if missing).
    const QCommandLineOption style(QStringLiteral("widget-style"),
        QCoreApplication::translate("kk::CommandLine",
            "Widget style for this run (e.g. Breeze, Fusion); the stored choice is kept."),
        QStringLiteral("key"));
    const QCommandLineOption colorScheme(QStringLiteral("color-scheme"),
        QCoreApplication::translate("kk::CommandLine",
            "KDE colour scheme for this run (e.g. BreezeDark, BreezeLight, system)."),
        QStringLiteral("id"));
    const QCommandLineOption render(QStringLiteral("render"),
        QCoreApplication::translate("kk::CommandLine",
            "Export the timeline of <project> to a file, then exit. Usage: --render "
            "project.kkproj <out-file> [--preset id]. Without --preset the format follows the "
            "file extension (mp4, mov, mkv, webm, wav, flac, mp3, m4a, opus, gif, png, jpg; "
            "name_%05d.png etc. for image sequences)."),
        QStringLiteral("project"));
    const QCommandLineOption preset(QStringLiteral("preset"),
        QCoreApplication::translate("kk::CommandLine",
            "Render preset id for --render (e.g. youtube_1080p, master_prores_hq, audio_wav)."),
        QStringLiteral("id"));
    const QCommandLineOption menuReference(QStringLiteral("menu-reference"),
        QCoreApplication::translate("kk::CommandLine",
            "Write the menu and panel reference (every menu entry with its default shortcut "
            "and what it does, every panel of every page) as Markdown to <file>, then exit. "
            "Uses default settings; the user's settings are neither read nor changed. This is "
            "how docs/menu-reference.md is made."),
        QStringLiteral("file"));
    parser.addOption(render);
    parser.addOption(preset);
    parser.addOption(menuReference);
    parser.addOption(screenshot);
    parser.addOption(style);
    parser.addOption(colorScheme);
    parser.addOption(resetLayout);
    parser.addOption(renderFrame);

    parser.process(arguments); // exits on --help / --version / unknown options

    QStringList positional = parser.positionalArguments();
    options.resetLayout = parser.isSet(resetLayout);
    options.style = parser.value(style);
    options.colorScheme = parser.value(colorScheme);
    options.menuReferenceFile = parser.value(menuReference);
    if (parser.isSet(screenshot)) {
        options.screenshotPage = parser.value(screenshot);
        if (positional.isEmpty()) {
            if (error)
                *error = QStringLiteral("--screenshot needs an output file: --screenshot "
                                        "<page> <out.png> [project]");
            return false;
        }
        options.screenshotPath = positional.takeFirst();
    }
    if (parser.isSet(render)) {
        options.renderProject = parser.value(render);
        options.renderPreset = parser.value(preset);
        if (positional.size() != 1) {
            if (error)
                *error
                    = QStringLiteral("usage: --render <project.kkproj> <out-file> [--preset id]");
            return false;
        }
        options.renderOutput = positional.first();
        return true;
    }
    if (parser.isSet(renderFrame)) {
        bool ok = false;
        options.renderFrameProject = parser.value(renderFrame);
        if (positional.size() == 2)
            options.renderFrameIndex = positional.first().toLongLong(&ok);
        if (!ok || options.renderFrameIndex < 0) {
            if (error)
                *error = QStringLiteral("usage: --render-frame <project.kkproj> <frame> <out.png>");
            return false;
        }
        options.renderFramePath = positional.last();
        return true;
    }
    if (!positional.isEmpty())
        options.projectFile = positional.takeFirst();
    if (!positional.isEmpty()) {
        if (error)
            *error
                = QStringLiteral("unexpected arguments: %1").arg(positional.join(QLatin1Char(' ')));
        return false;
    }
    return true;
}

} // namespace kk
