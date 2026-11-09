// SPDX-License-Identifier: GPL-3.0-or-later
// CliRunner — `kinoklip <command> --param value ...`: the command-line front end of the command
// layer (app/commands/Command.h). It only turns text into a JSON call and the result into text;
// the commands themselves know nothing about it.
#pragma once

#include <QStringList>

#include <cstdio>

namespace kk::cli {

/// True if @p arguments (without the program name) start with a command-line command: `commands`,
/// `help`, or the first word of a registered command (`clip`, `render`, `version`, ...).
/// A first argument that is an existing file is a project to open, not a command.
[[nodiscard]] bool isCommand(const QStringList& arguments);

/// Runs the command in @p arguments (without the program name): normal output to @p out,
/// messages and progress to @p err. Returns the exit code: 0 success, 1 the command failed,
/// 2 the command line is wrong (unknown command or option, missing or bad value).
/// Needs a QGuiApplication (offscreen is fine) and Resources initialised (kk::initResources()).
int run(const QStringList& arguments, std::FILE* out, std::FILE* err);

} // namespace kk::cli
