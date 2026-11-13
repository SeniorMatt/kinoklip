// SPDX-License-Identifier: GPL-3.0-or-later
// The interface language: which translation files are loaded at start-up.
#pragma once

#include <QList>
#include <QString>

namespace kk::i18n {

/// Stored choice: "system" (follow the desktop) or a language code from languages().
inline constexpr char kLanguageKey[] = "ui/language";

struct Language
{
    QString code; ///< "en", "de", "es", "ru", "uk"
    QString name; ///< the language's own name, as the Language combo shows it
};

/// English plus every language Kinoklip ships a translation for.
QList<Language> languages();

/// The language to use: @p commandLine (--lang) if set, else the KK_LANG environment variable,
/// else the stored choice (QSettings ui/language); "system" and unknown values follow the
/// desktop's UI languages. Always one of the codes in languages().
QString resolveLanguage(const QString& commandLine = QString());

/// Installs the application's translator and Qt's own (qtbase: standard buttons, file
/// dialog). Call right after the QApplication exists, before any widget. English installs
/// nothing. Returns the language that was set up.
QString install(const QString& commandLine = QString());

/// Installs the translators for @p code (no settings, no environment). For tests.
void installLanguage(const QString& code);

} // namespace kk::i18n
