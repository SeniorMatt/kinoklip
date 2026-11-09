// SPDX-License-Identifier: GPL-3.0-or-later
// Media import helper with user feedback.
#include "ui/common/MediaImport.h"

#include "core/MediaPool.h"
#include "core/Plural.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "media/MediaSource.h"
#include "pack/EmbeddedMedia.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaStore.h"
#include "ui/dialogs/ProjectSettingsDialog.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>

#include <algorithm>

namespace kk {

namespace {
QString tr(const char* text)
{
    return QCoreApplication::translate("kk::MediaImport", text);
}

constexpr int kMaxListedWarnings = 12; // keep the dialog a sensible size

std::function<bool(Rational, Rational)>& frameRatePromptHandler()
{
    static std::function<bool(Rational, Rational)> handler;
    return handler;
}

QString rateText(Rational fps)
{
    return QString::number(fps.toDouble(), 'g', 5);
}
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

QString otioFileDialogFilter()
{
    return tr("OpenTimelineIO files (*.otio);;All files (*)");
}

QString subtitleFileDialogFilter()
{
    return tr("Subtitle files (*.srt *.vtt *.SRT *.VTT);;All files (*)");
}

QString importTargetBin(const AppContext& ctx)
{
    if (prefs::importIntoMasterBin() || !ctx.currentMediaPoolBin)
        return {};
    return ctx.currentMediaPoolBin();
}

QStringList importMediaWithFeedback(
    AppContext& ctx, const QStringList& paths, QWidget* parent, const std::optional<QString>& binId)
{
    if (paths.isEmpty() || !ctx.project)
        return {};
    // A single-file project copies the files in first and imports the stored copies.
    QStringList toImport = paths;
    if (ctx.mediaStore && ctx.mediaStore->storesMedia()
        && !ctx.mediaStore->storeMedia(toImport, parent))
        return {};
    const bool firstImport = hasNoMediaYet(ctx);
    const ImportResult result
        = ctx.project->importMedia(toImport, binId ? *binId : importTargetBin(ctx));
    if (!result.warnings.isEmpty()) {
        QStringList shown = result.warnings.mid(0, kMaxListedWarnings);
        if (result.warnings.size() > kMaxListedWarnings)
            shown.append(tr("… and %1 more").arg(result.warnings.size() - kMaxListedWarnings));
        QMessageBox box(QMessageBox::Warning, tr("Import Media"),
            plural(paths.size(), tr("%1 of %2 file could not be imported."),
                tr("%1 of %2 files could not be imported."))
                .arg(result.warnings.size())
                .arg(paths.size()),
            QMessageBox::Ok, parent);
        box.setDetailedText((result.warnings + result.notes).join(QLatin1Char('\n')));
        box.setInformativeText((shown + result.notes).join(QLatin1Char('\n')));
        box.exec();
    } else if (!result.notes.isEmpty()) {
        // Imported, but with something the user should know (empty subtitle cues skipped).
        QMessageBox box(QMessageBox::Information, tr("Import Media"),
            plural(result.importedIds.size(), tr("%1 file imported."), tr("%1 files imported."))
                .arg(result.importedIds.size()),
            QMessageBox::Ok, parent);
        box.setInformativeText(result.notes.join(QLatin1Char('\n')));
        box.exec();
    }
    offerProjectFrameRate(ctx, firstImport, result.importedIds, parent);
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

bool hasNoMediaYet(const AppContext& ctx)
{
    if (!ctx.project)
        return false;
    const auto& items = ctx.project->mediaPool()->state().items;
    return std::none_of(items.cbegin(), items.cend(),
        [](const MediaItem& m) { return !m.isTimeline() && !m.isSubtitle(); });
}

bool offerProjectFrameRate(
    AppContext& ctx, bool firstImport, const QStringList& importedIds, QWidget* parent)
{
    if (!firstImport || !ctx.project)
        return false;
    const MediaPool* pool = ctx.project->mediaPool();
    Rational clipRate {0, 1};
    for (const QString& id : importedIds) {
        const MediaItem* m = pool->item(id);
        if (m && !m->isTimeline() && m->info.hasVideo && !m->info.isStillImage
            && m->info.frameRate.isValid()) {
            clipRate = m->info.frameRate;
            break;
        }
    }
    ProjectSettings settings = ctx.project->settings();
    if (!clipRate.isValid() || clipRate == settings.fps
        || !ProjectSettingsDialog::frameRates().contains(clipRate))
        return false;
    // Only while the rate can change: no timeline that follows the project has clips.
    for (const quint64 uid : ctx.project->timelineUids()) {
        const auto data = ctx.project->timelineData(uid);
        if (data && data->useProjectSettings && data->duration() > 0)
            return false;
    }
    bool change = false;
    if (frameRatePromptHandler()) {
        change = frameRatePromptHandler()(clipRate, settings.fps);
    } else {
        QMessageBox box(QMessageBox::Question, tr("Change Project Frame Rate"),
            tr("The clips you added run at %1 fps, the project at %2 fps.")
                .arg(rateText(clipRate), rateText(settings.fps)),
            QMessageBox::NoButton, parent);
        box.setInformativeText(tr("Change the project frame rate to %1 fps to match them? "
                                  "Once a timeline has clips, its frame rate cannot change.")
                                   .arg(rateText(clipRate)));
        QPushButton* changeButton = box.addButton(tr("Change"), QMessageBox::AcceptRole);
        box.addButton(tr("Don't Change"), QMessageBox::RejectRole);
        box.setDefaultButton(changeButton);
        box.exec();
        change = box.clickedButton() == changeButton;
    }
    if (!change)
        return false;
    settings.fps = clipRate;
    ctx.project->setSettings(settings);
    return true;
}

void setFrameRatePromptHandlerForTests(std::function<bool(Rational, Rational)> handler)
{
    frameRatePromptHandler() = std::move(handler);
}

} // namespace kk
