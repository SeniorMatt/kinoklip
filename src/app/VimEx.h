// SPDX-License-Identifier: GPL-3.0-or-later
// VimEx — the ex commands of the `:` line (spec section 83): the table of built-in commands with
// their shortest abbreviations (as in Neovim, `:w` is :write, `:bn` is :bnext), the parsing of a
// line into command, bang and argument, and the completion of the line (cmdline.txt). The
// running is the window's (VimController); this part has no widgets.
#pragma once

#include "ui/common/VimCommandLine.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace kk::vimex {

/// What the argument of a command is, for completion.
enum class Arg {
    None,
    Action, ///< an action id or name (`:action edit.undo`)
    Registry, ///< the rest of a registry command (`marker add --frame 120`)
    Page,
    Panel,
    Timeline, ///< a timeline name
    Option, ///< `:set` words
    File, ///< a path
    Help, ///< a help topic
    Text
};

struct Command
{
    QString name; ///< the full name ("write")
    int shortest = 1; ///< shortest abbreviation that works ("w")
    Arg arg = Arg::None;
    QString description;
};

/// The built-in ex commands.
[[nodiscard]] const QVector<Command>& commands();

/// A line split up.
struct Parsed
{
    /// The built-in command the word names (full name), empty if it is not one.
    QString name;
    /// The word as typed ("w", "bn", "marker").
    QString word;
    bool bang = false; ///< `:q!`
    QString arg; ///< the rest, trimmed
    /// `:120`, `:+10`, `:-5`, `:$`, `:00:01:10:00`: a go-to-time line (name = "goto").
    bool isGoto = false;
};

/// Splits @p line (without the colon). A word that is a unique abbreviation of a built-in
/// command gets its full name.
[[nodiscard]] Parsed parse(const QString& line);

/// What completion may offer.
struct Sources
{
    std::function<QStringList()> actionIds;
    std::function<QStringList()> registryCommands; ///< "marker add", "clip trim", "render" ...
    std::function<QStringList()> timelines;
    std::function<QStringList()> timecodes; ///< marker and playhead timecodes
    std::function<QStringList()> helpTopics;
};

/// Matches for the text before the cursor.
[[nodiscard]] VimCompletion complete(const QString& before, const Sources& sources);

/// The pages (`:page edit`), in the order of `gt`.
[[nodiscard]] QStringList pageNames();
/// The panels `:panel` and <leader>w know, as "name" -> panel key.
[[nodiscard]] QStringList panelNames();
/// The names `:set` knows.
[[nodiscard]] QStringList optionNames();

} // namespace kk::vimex
