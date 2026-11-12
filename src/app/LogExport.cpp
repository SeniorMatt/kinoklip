// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/LogExport.h"

#include "core/MemoryReport.h"
#include "render/Compositor.h"

#include <QCoreApplication>
#include <QDate>
#include <QSaveFile>

namespace kk::logexport {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::logexport", text);
}

/// Section titles stay English in the file (a person reading a bug report may not read the
/// sender's language); only the dialog text around them is translated.
QString title(int index)
{
    static const QStringList titles {
        QStringLiteral("System"),
        QStringLiteral("Decoder used for each media file"),
        QStringLiteral("Playback statistics"),
        QStringLiteral("Warnings and errors"),
        QStringLiteral("Other messages"),
        QStringLiteral("Memory"),
    };
    return titles.at(index);
}

QString typeName(QtMsgType type)
{
    switch (type) {
    case QtWarningMsg:
        return QStringLiteral("warning");
    case QtCriticalMsg:
        return QStringLiteral("error");
    case QtFatalMsg:
        return QStringLiteral("fatal");
    default:
        return QStringLiteral("info");
    }
}

QString clock(qint64 ms)
{
    return QStringLiteral("%1.%2").arg(ms / 1000, 5).arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QString line(const sessionlog::Entry& e)
{
    QString text = shorten(e.text);
    text.replace(QStringLiteral("\n"), QStringLiteral("\n    "));
    return QStringLiteral("[%1] %2 %3: %4")
        .arg(clock(e.milliseconds), typeName(e.type), e.category, text);
}

bool isDecoderLine(const sessionlog::Entry& e)
{
    return e.type == QtInfoMsg && e.category == QLatin1String("kk.media")
        && (e.text.startsWith(QLatin1String("decoding "))
            || e.text.startsWith(QLatin1String("hardware decoding")));
}

bool isStatsLine(const sessionlog::Entry& e)
{
    return e.type == QtInfoMsg && e.category == QLatin1String("kk.playback")
        && (e.text.startsWith(QLatin1String("stats"))
            || e.text.startsWith(QLatin1String("cut look-ahead")));
}

bool isProblem(const sessionlog::Entry& e)
{
    return e.type == QtWarningMsg || e.type == QtCriticalMsg || e.type == QtFatalMsg;
}

QString duration(qint64 ms)
{
    const qint64 s = ms / 1000;
    if (s >= 3600)
        return QStringLiteral("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60);
    if (s >= 60)
        return QStringLiteral("%1 min %2 s").arg(s / 60).arg(s % 60);
    return QStringLiteral("%1 s").arg(s);
}

void addSection(QString& out, int index, const QStringList& lines, const QString& ifEmpty)
{
    out += QStringLiteral("\n== %1 ==\n").arg(title(index));
    if (lines.isEmpty())
        out += ifEmpty + QLatin1Char('\n');
    else
        out += lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace

QStringList sectionTitles()
{
    QStringList list;
    for (int i = 0; i < 6; ++i)
        list.append(title(i));
    return list;
}

QString shorten(const QString& text)
{
    return bugreport::scrub(bugreport::replaceHomeFolder(text, QStringLiteral("~")));
}

Session currentSession()
{
    sessionlog::RingBuffer& b = sessionlog::buffer();
    Session s;
    s.entries = b.entries();
    s.dropped = b.dropped();
    s.elapsedMilliseconds = b.elapsedMilliseconds();
    return s;
}

QString build(const bugreport::SystemInfo& info, const Session& session, const QDateTime& when)
{
    QString out;
    out += QStringLiteral("Kinoklip log file\n");
    out += QStringLiteral("Written %1 by Kinoklip %2.\n")
               .arg(when.toString(Qt::ISODate), shorten(info.version));
    out += QStringLiteral("Session length: %1.\n").arg(duration(session.elapsedMilliseconds));
    out += QStringLiteral(
        "This file was saved by the person using Kinoklip and was not sent anywhere. Paths under\n"
        "the home folder are written as ~/... and the user name and computer name are removed.\n"
        "It lists file names of media you used and the folders in some messages; read it before\n"
        "you send it.\n");
    if (session.dropped > 0)
        out += QStringLiteral(
            "\n%1 earlier lines dropped (the log keeps the last %2 lines or %3 MiB).\n")
                   .arg(session.dropped)
                   .arg(sessionlog::kMaxLines)
                   .arg(sessionlog::kMaxBytes / (1024 * 1024));

    QStringList system;
    for (const bugreport::Line& l : info.lines)
        system.append(shorten(QStringLiteral("%1: %2").arg(l.first, l.second)));
    addSection(out, 0, system, QStringLiteral("(nothing)"));

    QStringList decoders;
    QStringList stats;
    QStringList problems;
    QStringList other;
    for (const sessionlog::Entry& e : session.entries) {
        if (isDecoderLine(e))
            decoders.append(line(e));
        else if (isStatsLine(e))
            stats.append(line(e));
        else if (isProblem(e))
            problems.append(line(e));
        else
            other.append(line(e));
    }
    addSection(
        out, 1, decoders, QStringLiteral("(no media was opened for playback in this session)"));
    addSection(out, 2, stats, QStringLiteral("(nothing was played in this session)"));
    addSection(out, 3, problems, QStringLiteral("(none)"));
    addSection(out, 4, other, QStringLiteral("(none)"));
    QStringList memory = memreport::lines();
    memory += Compositor::liveStats();
    addSection(out, 5, memory, QStringLiteral("(not available on this system)"));
    return out;
}

QString buildCurrent()
{
    return build(bugreport::collect(), currentSession());
}

QString contentsDescription()
{
    QString text
        = tr("Saves what Kinoklip did in this session as one text file that you can attach "
             "to a bug report. The log is kept in memory while you work and written only "
             "now, to the file you choose; nothing is sent anywhere.");
    text += QStringLiteral("\n\n") + tr("The file contains:");
    const QStringList items {
        tr("Kinoklip version, operating system, desktop (Wayland or X11), graphics card and "
           "OpenGL renderer"),
        tr("The decoder chosen for each media file (hardware or software)"),
        tr("The playback statistics (frames shown and dropped, render times)"),
        tr("The warnings and errors of this session"),
        tr("Other messages Kinoklip printed (the last %1 lines at most)")
            .arg(sessionlog::kMaxLines),
        tr("Memory when the file was saved: the process and system memory, what the graphics "
           "driver counts for Kinoklip, and what the renderers hold"),
    };
    const QStringList titles = sectionTitles();
    for (int i = 0; i < items.size(); ++i)
        text += QStringLiteral("\n  %1. %2: %3").arg(i + 1).arg(titles.at(i), items.at(i));
    text += QStringLiteral("\n\n")
        + tr("It names the media files you used, and some messages contain folder paths. Paths "
             "under your home folder are written as ~/..., your user name and computer name are "
             "removed, and no project content, media content or settings are included. "
             "You can read the whole file below before you save it.");
    return text;
}

QString defaultFileName(const QDate& date)
{
    return QStringLiteral("kinoklip-log-%1.txt").arg(date.toString(Qt::ISODate));
}

bool exportToFile(const QString& path, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(buildCurrent().toUtf8()) < 0
        || !file.commit()) {
        if (error)
            *error = QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

} // namespace kk::logexport
