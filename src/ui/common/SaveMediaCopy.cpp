// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/SaveMediaCopy.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"
#include "platform/FileDialogs.h"
#include "ui/common/AppContext.h"
#include "ui/common/ProgressTask.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>

#include <atomic>

namespace kk {

namespace {
constexpr char kLastFolderKey[] = "paths/saveCopyDir";

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::SaveMediaCopy", text);
}
} // namespace

QVector<StoredCopySource> storedCopySources(const MediaPool& pool, const QStringList& ids)
{
    QVector<StoredCopySource> out;
    for (const QString& id : ids) {
        const MediaItem* item = pool.item(id);
        if (!item || item->isTimeline() || !pack::mayBeEmbeddedPath(item->path))
            continue;
        StoredCopySource s;
        if (!pack::splitEmbeddedPath(item->path, &s.container, &s.entry))
            continue;
        s.mediaId = id;
        s.fileName = QFileInfo(s.entry).fileName();
        if (s.fileName.isEmpty())
            continue;
        out.append(s);
    }
    return out;
}

QString defaultCopyFolder(
    const QString& originalFolder, const QString& typeFolder, const QString& projectFolder)
{
    for (const QString& dir : {originalFolder, typeFolder, projectFolder}) {
        if (!dir.isEmpty() && QFileInfo(dir).isDir())
            return QDir::cleanPath(dir);
    }
    return QDir::homePath();
}

QString typeFolderFor(const MediaItem& item)
{
    if (item.info.hasVideo)
        return QStandardPaths::writableLocation(item.info.isStillImage
                ? QStandardPaths::PicturesLocation
                : QStandardPaths::MoviesLocation);
    if (item.info.hasAudio)
        return QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
}

QString uniqueCopyName(const QString& folder, const QString& fileName, const QSet<QString>& taken)
{
    const QDir dir(folder);
    const auto free = [&](const QString& name) {
        return !taken.contains(name) && !QFileInfo::exists(dir.absoluteFilePath(name));
    };
    if (free(fileName))
        return fileName;
    const QFileInfo info(fileName);
    const QString suffix = info.suffix();
    // "archive.tar.gz" keeps only the last suffix apart: "archive.tar (2).gz" is fine here.
    const QString base
        = suffix.isEmpty() ? fileName : fileName.left(fileName.size() - suffix.size() - 1);
    for (int n = 2;; ++n) {
        const QString name = base + QStringLiteral(" (%1)").arg(n)
            + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
        if (free(name))
            return name;
    }
}

int saveCopiesToComputer(AppContext& ctx, const QStringList& ids, QWidget* parent)
{
    if (!ctx.project)
        return 0;
    const MediaPool& pool = *ctx.project->mediaPool();
    const QVector<StoredCopySource> sources = storedCopySources(pool, ids);
    if (sources.isEmpty())
        return 0;
    const QString title
        = sources.size() == 1 ? tr("Save a Copy to Computer") : tr("Save Copies to Computer");
    const MediaItem* first = pool.item(sources.front().mediaId);
    const QString projectFolder = QFileInfo(sources.front().container).absolutePath();
    const QString folder
        = defaultCopyFolder({}, first ? typeFolderFor(*first) : QString(), projectFolder);

    // Where each file goes.
    QStringList targets;
    if (sources.size() == 1) {
        const QString chosen = platform::getSaveFileName(parent, title, QString(),
            QLatin1String(kLastFolderKey), sources.front().fileName, folder);
        if (chosen.isEmpty())
            return 0;
        targets.append(chosen);
    } else {
        const QString chosen
            = platform::getExistingDirectory(parent, title, QLatin1String(kLastFolderKey), folder);
        if (chosen.isEmpty())
            return 0;
        QSet<QString> taken;
        for (const StoredCopySource& s : sources) {
            const QString name = uniqueCopyName(chosen, s.fileName, taken);
            taken.insert(name);
            targets.append(QDir(chosen).absoluteFilePath(name));
        }
    }

    // Copy on a worker thread; the progress covers all files.
    qint64 total = 0;
    for (const StoredCopySource& s : sources) {
        pack::EntryLocation loc;
        if (pack::locateEntry(s.container, s.entry, &loc))
            total += loc.size;
    }
    int written = 0;
    QString failure;
    std::atomic<bool> cancelled {false};
    const bool finished = runWithProgress(parent, title,
        sources.size() == 1 ? tr("Copying %1…").arg(sources.front().fileName)
                            : tr("Copying %1 files…").arg(sources.size()),
        [&](const TaskProgress& report) {
            qint64 before = 0;
            for (qsizetype i = 0; i < sources.size(); ++i) {
                pack::EntryLocation loc;
                pack::locateEntry(sources[i].container, sources[i].entry, &loc);
                const pack::ExtractResult r
                    = pack::extractMedia(sources[i].container, sources[i].entry, targets[i],
                        [&](qint64 done, qint64) { return report(before + done, total); });
                if (r.status == pack::Status::Cancelled) {
                    cancelled = true;
                    return;
                }
                if (r.status != pack::Status::Ok) {
                    failure = r.error;
                    return;
                }
                before += loc.size;
                ++written;
            }
        });
    if (!finished || cancelled)
        return written;
    if (!failure.isEmpty()) {
        QMessageBox::warning(parent, title, tr("The copy could not be completed: %1").arg(failure));
    }
    return written;
}

} // namespace kk
