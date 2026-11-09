// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineImport implementation.
#include "app/TimelineImport.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "export/Otio.h"
#include "platform/FileDialogs.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaImport.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QWidget>

namespace kk {

namespace {

constexpr char kLastTimelineDirKey[] = "paths/lastTimelineImportDir";

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::TimelineImport", text);
}

/// An offline media pool item for a file the .otio names but that is not there: the probe result
/// is made up from what the file says, like the cached probe of a project whose media is gone.
QString addOfflineItem(
    Project& project, const otio::MediaRef& ref, const QString& path, const QString& binId)
{
    const ProjectSettings settings = project.settings();
    MediaItem item;
    item.path = path;
    item.name = QFileInfo(path).fileName();
    item.kind = MediaItemKind::File;
    item.info.valid = true;
    item.info.path = path;
    item.info.hasVideo = ref.hasVideo;
    item.info.hasAudio = ref.hasAudio;
    item.info.width = item.info.displayWidth = settings.width;
    item.info.height = item.info.displayHeight = settings.height;
    item.info.frameRate = ref.hasVideo && ref.frameRate.isValid() ? ref.frameRate : Rational {0, 1};
    item.info.durationUs = ref.durationUs;
    item.info.audioChannels = ref.hasAudio ? 2 : 0;
    item.info.audioSampleRate = ref.hasAudio ? 48000 : 0;
    item.binId = binId;
    return project.mediaPool()->addItem(std::move(item));
}

} // namespace

TimelineImportReport importOtioFile(Project& project, const QString& path, const QString& binId)
{
    TimelineImportReport report;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        report.error = tr("Cannot read %1: %2").arg(path, file.errorString());
        return report;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    const QFileInfo info(path);
    const ProjectSettings settings = project.settings();
    otio::ImportOptions options;
    options.baseDir = info.absolutePath();
    options.fallbackName = info.completeBaseName();
    options.defaultFps = settings.fps;
    options.width = settings.width;
    options.height = settings.height;

    // The media is added inside the same undo step as the timeline.
    project.beginEdit(tr("Import Timeline"));
    MediaPool* pool = project.mediaPool();
    options.resolveMedia = [&](const otio::MediaRef& ref) {
        // A clip without a file keeps its name as an offline file next to the .otio file.
        const QString mediaPath = !ref.path.isEmpty()
            ? ref.path
            : QDir(info.absolutePath())
                  .absoluteFilePath(ref.name.isEmpty() ? tr("Offline clip") : ref.name);
        if (const MediaItem* existing = pool->itemByPath(mediaPath))
            return otio::ResolvedMedia {existing->id, existing->info.durationUs};
        if (QFileInfo::exists(mediaPath)) {
            const ImportResult imported = project.importMedia({mediaPath}, binId);
            if (!imported.importedIds.isEmpty()) {
                ++report.mediaAdded;
                const MediaItem* item = pool->item(imported.importedIds.front());
                return otio::ResolvedMedia {
                    imported.importedIds.front(), item ? item->info.durationUs : 0};
            }
            // A file that cannot be probed (not media): offline like a missing one.
        }
        report.offline << mediaPath;
        const QString id = addOfflineItem(project, ref, mediaPath, binId);
        return otio::ResolvedMedia {id, ref.durationUs};
    };
    otio::TimelineImport imported = otio::importOtio(bytes, options);
    if (!imported.ok) {
        project.cancelEdit();
        report.error = imported.error;
        return report;
    }
    report.clipCount = imported.clipCount;
    report.skipped = imported.skipped;
    report.uid = project.importTimeline(std::move(imported.timeline), binId);
    project.endEdit();
    return report;
}

void connectTimelineImportActions(AppContext& ctx, QWidget* window)
{
    QObject::connect(ctx.actions->action(QStringLiteral("file.import_timeline")),
        &QAction::triggered, window, [&ctx, window] {
            const QString path = platform::getOpenFileName(window, tr("Import Timeline"),
                otioFileDialogFilter(), QLatin1String(kLastTimelineDirKey));
            if (path.isEmpty())
                return;
            const TimelineImportReport report
                = importOtioFile(*ctx.project, path, importTargetBin(ctx));
            if (!report.uid) {
                QMessageBox::warning(window, tr("Import Timeline"), report.error);
                return;
            }
            ctx.project->setActiveTimeline(report.uid);
            QStringList lines;
            if (!report.offline.isEmpty())
                lines << tr("%1 file(s) were not found; their clips are offline:")
                             .arg(report.offline.size())
                      << report.offline.mid(0, 10);
            if (!report.skipped.isEmpty())
                lines << tr("Left out of the import:") << report.skipped;
            if (!lines.isEmpty())
                QMessageBox::information(
                    window, tr("Import Timeline"), lines.join(QLatin1Char('\n')));
        });
}

} // namespace kk
