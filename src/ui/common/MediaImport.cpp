// SPDX-License-Identifier: GPL-3.0-or-later
// Media import helper with user feedback.
#include "ui/common/MediaImport.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "media/MediaSource.h"
#include "pack/EmbeddedMedia.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaStore.h"

#include <QCoreApplication>
#include <QFileInfo>
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
                 "jpeg", "tif", "tiff", "bmp", "webp", "srt", "vtt"}) {
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

QString subtitleFileDialogFilter()
{
    return tr("Subtitle files (*.srt *.vtt *.SRT *.VTT);;All files (*)");
}

QStringList importMediaWithFeedback(
    AppContext& ctx, const QStringList& paths, QWidget* parent, const QString& binId)
{
    if (paths.isEmpty() || !ctx.project)
        return {};
    // A single-file project copies the files in first and imports the stored copies.
    QStringList toImport = paths;
    if (ctx.mediaStore && ctx.mediaStore->storesMedia()
        && !ctx.mediaStore->storeMedia(toImport, parent))
        return {};
    const ImportResult result = ctx.project->importMedia(toImport, binId);
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

int storeItemsInProject(AppContext& ctx, const QStringList& ids, QWidget* parent)
{
    if (!ctx.project || !ctx.mediaStore || !ctx.mediaStore->storesMedia())
        return 0;
    MediaPool* pool = ctx.project->mediaPool();
    QStringList itemIds;
    QStringList paths;
    for (const MediaItem& m : pool->state().items) {
        if (m.isTimeline() || m.path.isEmpty() || (!ids.isEmpty() && !ids.contains(m.id)))
            continue;
        if (!isLinkedFile(m.path))
            continue;
        itemIds.append(m.id);
        paths.append(m.path);
    }
    if (paths.isEmpty())
        return 0;
    QStringList stored = paths;
    if (!ctx.mediaStore->storeMedia(stored, parent))
        return 0;
    int changed = 0;
    ctx.project->beginEdit(tr("Store Media in Project"));
    for (qsizetype i = 0; i < itemIds.size() && i < stored.size(); ++i) {
        if (stored[i] != paths[i] && pool->relinkItem(itemIds[i], stored[i]))
            ++changed;
    }
    ctx.project->endEdit();
    return changed;
}

bool isLinkedFile(const QString& path)
{
    // Embedded media first (a cheap string test), then one stat for plain files.
    return !isStoredInProject(path) && media::mediaExists(path);
}

bool isStoredInProject(const QString& path)
{
    if (!pack::mayBeEmbeddedPath(path))
        return false;
    QString container;
    QString entry;
    return pack::splitEmbeddedPath(path, &container, &entry);
}

QString mediaLocationText(const QString& path)
{
    QString container;
    QString entry;
    if (pack::mayBeEmbeddedPath(path) && pack::splitEmbeddedPath(path, &container, &entry))
        return tr("Stored in the project file (%1)").arg(QFileInfo(container).fileName());
    return tr("Linked");
}

} // namespace kk
