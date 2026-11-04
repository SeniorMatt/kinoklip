// SPDX-License-Identifier: GPL-3.0-or-later
// ForeignKeymaps — reads the keyboard shortcut files of other editors (Premiere Pro .kys,
// Final Cut Pro .commandset) into Kinoklip action ids and keys.
#pragma once

#include <QKeySequence>
#include <QString>
#include <QStringList>
#include <QVector>

namespace kk::foreignkeys {

/// One shortcut of a foreign file that maps to a Kinoklip command.
struct Binding
{
    QString command; ///< the command name as written in the file
    QString actionId; ///< the Kinoklip action ("edit.split")
    QKeySequence key; ///< in Kinoklip's terms (Final Cut's Command is Ctrl)
};

/// What readFile() found.
struct Result
{
    QString format; ///< display name of the editor ("Premiere Pro")
    QString basedOn; ///< built-in preset the profile starts from ("premiere-pro")
    QVector<Binding> bindings; ///< recognised shortcuts, in file order
    int skipped = 0; ///< shortcuts of commands Kinoklip does not have, or keys it cannot read
    QStringList skippedCommands; ///< their command names (for the log / tests)
};

/// True for the file kinds readFile() reads (by extension: .kys, .commandset).
[[nodiscard]] bool isForeignKeymapFile(const QString& path);

/// Reads a Premiere Pro keyboard file (.kys, XML) or a Final Cut Pro command set (.commandset,
/// an XML property list). Both are parsed defensively from their general shape (an entry that
/// names a command next to a key and its modifiers), not from a fixed schema: entries it does
/// not understand are counted in Result::skipped, never an error. Returns false (and sets
/// @p error) only when the file cannot be read at all: missing, not XML, a binary property
/// list (convert it with `plutil -convert xml1`).
bool readFile(const QString& path, Result& out, QString* error = nullptr);

/// The Kinoklip action for a foreign command name ("cmd.edit.razorAtCTI", "Blade", "SetIn"),
/// or empty. Case, spaces and punctuation do not matter; known prefixes are ignored.
[[nodiscard]] QString actionForCommand(const QString& command);

} // namespace kk::foreignkeys
