// SPDX-License-Identifier: GPL-3.0-or-later
// Media import helper with user feedback.
#include "ui/common/MediaImport.h"

#include "core/Project.h"
#include "ui/common/AppContext.h"

#include <QCoreApplication>
#include <QMessageBox>

namespace kk {

namespace {
QString tr(const char* text)
{
    return QCoreApplication::translate("kk::MediaImport", text);
}

constexpr int kMaxListedWarnings = 12; // keep the dialog a sensible size
} // namespace

QStringList mediaFileNamePatterns()
{
    static const QStringList patterns = [] {
        QStringList list;
        for (const char* ext : {"mp4", "mov", "mkv", "webm", "avi", "mxf", "m4v", "mts", "m2ts",
                 "mpg", "mpeg", "wav", "mp3", "flac", "ogg", "m4a", "aac", "opus", "png", "jpg",
                 "jpeg", "tif", "tiff", "bmp", "webp"}) {
            list.append(QStringLiteral("*.") + QLatin1String(ext));
            list.append(QStringLiteral("*.") + QLatin1String(ext).toString().toUpper());
        }
        return list;
    }();
    return patterns;
}

QString mediaFileDialogFilter()
{
    QStringList lower;
    for (const QString& p : mediaFileNamePatterns()) {
        if (p == p.toLower())
            lower.append(p);
    }
    return tr("Media files (%1);;All files (*)").arg(lower.join(QLatin1Char(' ')));
}

QStringList importMediaWithFeedback(
    AppContext& ctx, const QStringList& paths, QWidget* parent, const QString& binId)
{
    if (paths.isEmpty() || !ctx.project)
        return {};
    const ImportResult result = ctx.project->importMedia(paths, binId);
    if (!result.warnings.isEmpty()) {
        QStringList shown = result.warnings.mid(0, kMaxListedWarnings);
        if (result.warnings.size() > kMaxListedWarnings)
            shown.append(tr("… and %1 more").arg(result.warnings.size() - kMaxListedWarnings));
        QMessageBox box(QMessageBox::Warning, tr("Import Media"),
            tr("%1 of %2 file(s) could not be imported.")
                .arg(result.warnings.size())
                .arg(paths.size()),
            QMessageBox::Ok, parent);
        box.setDetailedText(result.warnings.join(QLatin1Char('\n')));
        box.setInformativeText(shown.join(QLatin1Char('\n')));
        box.exec();
    }
    return result.importedIds;
}

} // namespace kk
