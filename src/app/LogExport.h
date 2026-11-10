// SPDX-License-Identifier: GPL-3.0-or-later
// LogExport — Help > Export Log File (spec section 64): the session log (core/SessionLog.h) and
// the system facts of Help > Report a Bug (app/BugReport.h) as one text file to attach to a
// bug report. Written only when the user asks; docs/privacy.md says what it contains.
#pragma once

#include "app/BugReport.h"
#include "core/SessionLog.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace kk::logexport {

/// The sections of the file, in order. The export dialog lists these same titles, so what the
/// dialog says is what the file has (a test compares).
[[nodiscard]] QStringList sectionTitles();

/// Replaces the home folder by "~" in @p text ("/home/anna/Videos/a.mp4" becomes
/// "~/Videos/a.mp4"; "/home/annabel" is another folder and stays), then removes the user name
/// and host name wherever they are left (bugreport::scrub()).
[[nodiscard]] QString shorten(const QString& text);

/// What the session log holds, for build().
struct Session
{
    QList<sessionlog::Entry> entries; ///< oldest first
    qint64 dropped = 0; ///< messages lost to the buffer limits
    qint64 elapsedMilliseconds = 0; ///< age of the session
};

/// The current session: the global buffer.
[[nodiscard]] Session currentSession();

/// The text of the file: a header, then the sections of sectionTitles(). Every message and
/// system line goes through shorten(). @p when is the time written in the header.
[[nodiscard]] QString build(const bugreport::SystemInfo& info, const Session& session,
    const QDateTime& when = QDateTime::currentDateTime());

/// build() of the current system and session.
[[nodiscard]] QString buildCurrent();

/// Paragraph(s) saying what the file contains and what it does not, shown by the export
/// dialog. Lists sectionTitles().
[[nodiscard]] QString contentsDescription();

/// "kinoklip-log-2026-11-10.txt".
[[nodiscard]] QString defaultFileName(const QDate& date = QDate::currentDate());

/// Writes buildCurrent() to @p path atomically (UTF-8). False with @p error set on failure.
bool exportToFile(const QString& path, QString* error);

} // namespace kk::logexport
