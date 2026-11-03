// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectController, single-file projects (docs/single-file-project.md, spec section 39): the
// open container and its lock, Save As between the two kinds, copying imported media in, and
// File > Pack / Unpack / Compact / Verify Project.
#include "app/AppInfo.h"
#include "app/AutosaveController.h"
#include "app/ProjectController.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "media/MediaCache.h"
#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"
#include "platform/FileDialogs.h"
#include "playback/PlaybackEngine.h"
#include "projects/ProjectLock.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/ProgressTask.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>

namespace kk {

namespace {
constexpr char kLastProjectDirKey[] = "paths/lastProjectDir";
constexpr int kMaxListed = 12; // names listed in a message box (the rest in its details)

QString native(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

QString sizeText(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes);
}

QString singleFileFilter()
{
    return QObject::tr("Single-file project (*.%1)").arg(QLatin1String(pack::kContainerSuffix));
}

/// The first kMaxListed lines of @p lines, plus "... and N more".
QString shortList(const QStringList& lines)
{
    QStringList shown = lines.mid(0, kMaxListed);
    if (lines.size() > kMaxListed)
        shown.append(QObject::tr("… and %1 more").arg(lines.size() - kMaxListed));
    return shown.join(QLatin1Char('\n'));
}

/// A container opened for this window, before it replaces the current one.
struct Held
{
    std::unique_ptr<pack::ProjectPackage> package;
    std::unique_ptr<ProjectLock> lock; ///< null = read-only
    QString readOnlyReason;
    bool lockWasCurrent = false; ///< the lock was the window's own (the same file again)
};

/// Takes the lock of @p path (or the window's own lock @p current if it is that file's) and
/// opens the container, writable if the lock was free and the file can be written.
bool openHeld(const QString& path, std::unique_ptr<ProjectLock>& current, Held* out, QString* error)
{
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (current && current->projectPath() == absolute) {
        out->lock = std::move(current);
        out->lockWasCurrent = true;
    } else {
        out->lock = ProjectLock::acquire(absolute);
    }
    if (!out->lock)
        out->readOnlyReason = QObject::tr("it is open in another window or program");
    else if (!QFileInfo(absolute).isWritable())
        out->readOnlyReason = QObject::tr("the file cannot be written here");
    const bool writable = out->readOnlyReason.isEmpty();
    QString why;
    out->package = pack::ProjectPackage::open(absolute, writable, &why);
    if (!out->package && writable) {
        // E.g. a read-only file system that still reports the file as writable.
        out->package = pack::ProjectPackage::open(absolute, false, &why);
        if (out->package)
            out->readOnlyReason = QObject::tr("the file cannot be written here");
    }
    if (!out->package) {
        if (out->lockWasCurrent)
            current = std::move(out->lock);
        if (error)
            *error = QObject::tr("Cannot open %1: %2").arg(native(absolute), why);
        return false;
    }
    if (!out->readOnlyReason.isEmpty() && !out->lockWasCurrent)
        out->lock.reset(); // read-only: nothing to protect
    else if (!out->readOnlyReason.isEmpty())
        current = std::move(out->lock);
    return true;
}

/// Rewrites a runtime document's media paths inside @p container to the same entry below
/// @p folder (the media an Unpack wrote there), so the document becomes a linked project.
QByteArray relinkToFolder(
    const QByteArray& runtime, const QString& container, const QString& folder)
{
    QJsonObject root = QJsonDocument::fromJson(runtime).object();
    QJsonObject pool = root.value(QLatin1String("mediaPool")).toObject();
    QJsonArray items = pool.value(QLatin1String("items")).toArray();
    const QString prefix = QDir::cleanPath(container) + QLatin1Char('/');
    const QDir dir(folder);
    for (qsizetype i = 0; i < items.size(); ++i) {
        QJsonObject item = items.at(i).toObject();
        const QString path = item.value(QLatin1String("path")).toString();
        if (!path.startsWith(prefix))
            continue;
        const QString entry = path.mid(prefix.size());
        item[QLatin1String("path")] = dir.absoluteFilePath(entry);
        item[QLatin1String("relativePath")] = entry;
        item.remove(QLatin1String("embedded"));
        items.replace(i, item);
    }
    pool[QLatin1String("items")] = items;
    root[QLatin1String("mediaPool")] = pool;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool writeFile(const QString& path, const QByteArray& bytes, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(native(path), file.errorString());
        return false;
    }
    return true;
}
} // namespace

// ---- The open container ----

bool ProjectController::isSingleFile() const
{
    return pack::isContainerFile(m_ctx.project->filePath());
}

bool ProjectController::openContainer(const QString& path, QString* error, QWidget* dialogParent)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    Held held;
    if (!openHeld(absolute, m_lock, &held, error))
        return false;
    const bool writable = held.readOnlyReason.isEmpty();
    // On failure the current project (and its container) stay as they are.
    const auto restoreLock = [&] {
        if (held.lockWasCurrent && held.lock)
            m_lock = std::move(held.lock);
    };
    QString why;
    if (!held.package->hasDocument()) {
        // A container without a document yet: an empty project named after the file.
        m_ctx.project->newProject();
        m_ctx.project->setName(QFileInfo(absolute).completeBaseName());
        m_ctx.project->markSaved(absolute);
    } else {
        const QByteArray document = held.package->document(&why);
        if (document.isEmpty() || !m_ctx.project->loadDocument(document, absolute, &why)) {
            restoreLock();
            if (error)
                *error = why.isEmpty()
                    ? tr("%1 has no readable project document.").arg(native(absolute))
                    : why;
            return false;
        }
    }
    m_package.reset();
    if (!held.lockWasCurrent)
        m_lock.reset();
    m_package = std::move(held.package);
    if (writable)
        m_lock = std::move(held.lock);
    m_readOnly = !writable;
    updatePackageActions();
    emit storageChanged();
    if (!writable) {
        notify(dialogParent, tr("Open Project"),
            tr("“%1” is opened read-only because %2. Changes can be saved under another name "
               "(Save Project As).")
                .arg(QFileInfo(absolute).fileName(), held.readOnlyReason));
    }
    return true;
}

bool ProjectController::attachContainer(const QString& path, QString* error)
{
    Held held;
    if (!openHeld(path, m_lock, &held, error)) {
        releaseContainer();
        m_readOnly = true; // the project is open, but its file cannot be saved in place
        updatePackageActions();
        return false;
    }
    const bool writable = held.readOnlyReason.isEmpty();
    m_package.reset();
    if (!held.lockWasCurrent)
        m_lock.reset();
    m_package = std::move(held.package);
    if (writable)
        m_lock = std::move(held.lock);
    m_readOnly = !writable;
    updatePackageActions();
    emit storageChanged();
    return true;
}

bool ProjectController::saveContainer(QString* error)
{
    const QString path = m_package->path();
    if (!m_package->saveDocument(m_ctx.project->documentJson(path), error))
        return false;
    pack::invalidateIndexCache(path);
    m_ctx.project->markSaved(path);
    return true;
}

void ProjectController::releaseContainer()
{
    const bool had = m_package || m_lock || m_readOnly;
    m_package.reset();
    m_lock.reset();
    m_readOnly = false;
    if (!had)
        return;
    updatePackageActions();
    emit storageChanged();
}

void ProjectController::updatePackageActions()
{
    ActionRegistry* actions = m_ctx.actions;
    if (!actions)
        return;
    const bool single = isSingleFile();
    const bool held = single && m_package;
    if (QAction* a = actions->action(QStringLiteral("file.pack_project")))
        a->setEnabled(!single);
    if (QAction* a = actions->action(QStringLiteral("file.unpack_project")))
        a->setEnabled(held);
    if (QAction* a = actions->action(QStringLiteral("file.compact_project")))
        a->setEnabled(held && !m_readOnly);
    if (QAction* a = actions->action(QStringLiteral("file.verify_project")))
        a->setEnabled(single && QFileInfo(m_ctx.project->filePath()).isFile());
}

void ProjectController::notify(QWidget* parent, const QString& title, const QString& text)
{
    // Not blocking: opening a project goes on (start-up, Project Manager, tests).
    auto* box = new QMessageBox(
        QMessageBox::Information, title, text, QMessageBox::Ok, parent ? parent : m_window);
    box->setObjectName(QStringLiteral("ProjectNotice"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->show();
}

// ---- MediaStore: imports into a single-file project ----

bool ProjectController::storesMedia() const
{
    return isSingleFile();
}

QString ProjectController::containerPath() const
{
    if (m_package)
        return m_package->path();
    return isSingleFile() ? QFileInfo(m_ctx.project->filePath()).absoluteFilePath() : QString();
}

bool ProjectController::storeMedia(QStringList& paths, QWidget* parent)
{
    if (!storesMedia())
        return true;
    QWidget* dialogParent = parent ? parent : m_window;
    if (!m_package || m_readOnly) {
        QMessageBox::warning(dialogParent, tr("Import Media"),
            tr("“%1” is open read-only: media cannot be copied into it. Save it under another "
               "name first (Save Project As).")
                .arg(QFileInfo(m_ctx.project->filePath()).fileName()));
        return false;
    }
    const QString container = m_package->path();
    QStringList sources;
    QVector<qsizetype> positions;
    for (qsizetype i = 0; i < paths.size(); ++i) {
        const QString& p = paths[i];
        QString owner;
        QString entry;
        if (pack::splitEmbeddedPath(p, &owner, &entry)
            && QFileInfo(owner).absoluteFilePath() == container)
            continue; // already inside this project
        // Folders and image sequence patterns cannot be stored: they stay linked.
        if (!pack::mayBeEmbeddedPath(p) && !QFileInfo(p).isFile())
            continue;
        sources.append(pack::mayBeEmbeddedPath(p) ? p : QFileInfo(p).absoluteFilePath());
        positions.append(i);
    }
    if (sources.isEmpty())
        return true;

    pack::ProjectPackage* package = m_package.get();
    QVector<pack::ProjectPackage::Added> added;
    pack::Status status = pack::Status::Failed;
    QString error;
    runWithProgress(dialogParent, tr("Import Media"),
        tr("Copying %n file(s) into the project…", nullptr, int(sources.size())),
        [&](const TaskProgress& progress) {
            status = package->addMedia(sources, &added, progress, &error);
        });
    pack::invalidateIndexCache(container);
    if (status == pack::Status::Cancelled)
        return false;
    if (status != pack::Status::Ok) {
        QMessageBox::critical(dialogParent, tr("Import Media"),
            tr("The media could not be copied into the project: %1").arg(error));
        return false;
    }
    QHash<QString, QString> stored;
    QStringList duplicates;
    for (const pack::ProjectPackage::Added& a : std::as_const(added)) {
        stored.insert(a.source, a.path);
        if (a.duplicate)
            duplicates.append(QFileInfo(a.source).fileName());
    }
    for (qsizetype k = 0; k < sources.size(); ++k) {
        const QString path = stored.value(sources[k]);
        if (!path.isEmpty())
            paths[positions[k]] = path;
    }
    if (!duplicates.isEmpty()) {
        notify(dialogParent, tr("Import Media"),
            tr("%n file(s) were already stored in this project and were not copied again:", nullptr,
                int(duplicates.size()))
                + QLatin1Char('\n') + shortList(duplicates));
    }
    // The file changed (newer than the last autosave): autosave the import soon, so crash
    // recovery does not take the autosave for an obsolete one.
    QTimer::singleShot(0, this, [this] {
        if (m_autosave)
            m_autosave->autosaveNow();
    });
    return true;
}

// ---- Save As between the kinds ----

bool ProjectController::saveAsSingleFile(const QString& target)
{
    const QString absolute = QFileInfo(target).absoluteFilePath();
    if (ProjectLock::isLocked(absolute)) {
        QMessageBox::warning(m_window, tr("Save Project As"),
            tr("%1 is open in another window or program.").arg(native(absolute)));
        return false;
    }
    // The current state as a linked document next to the target, then Pack it.
    const QFileInfo info(absolute);
    const QString temp = info.absolutePath() + QStringLiteral("/.") + info.completeBaseName()
        + QStringLiteral(".pack-%1.").arg(QCoreApplication::applicationPid())
        + QLatin1String(appinfo::projectExtension);
    QString error;
    if (!m_ctx.project->saveCopy(temp, &error)) {
        QMessageBox::critical(m_window, tr("Save Project As"), error);
        return false;
    }
    pack::PackResult result;
    runWithProgress(m_window, tr("Save Project As"),
        tr("Copying the project and its media into %1…").arg(info.fileName()),
        [&](const TaskProgress& progress) {
            result = pack::packProject(temp, absolute, progress);
        });
    QFile::remove(temp);
    if (result.status == pack::Status::Cancelled)
        return false;
    if (result.status != pack::Status::Ok) {
        QMessageBox::critical(m_window, tr("Save Project As"),
            tr("The project could not be packed: %1").arg(result.error));
        return false;
    }
    if (!openContainer(absolute, &error, m_window)) {
        QMessageBox::critical(m_window, tr("Save Project As"), error);
        return false;
    }
    if (!result.skipped.isEmpty()) {
        notify(m_window, tr("Save Project As"),
            tr("%n media file(s) stay linked (not stored in the project file):", nullptr,
                int(result.skipped.size()))
                + QLatin1Char('\n') + shortList(result.skipped));
    }
    return true;
}

bool ProjectController::saveAsLinked(const QString& kkprojTarget)
{
    if (!m_package) {
        QMessageBox::warning(m_window, tr("Save Project As"),
            tr("The project file is not available; save the project as a single file instead."));
        return false;
    }
    const QString target = QFileInfo(kkprojTarget).absoluteFilePath();
    const QString folder = QFileInfo(target).absolutePath();
    const QString container = m_package->path();
    // Unpack writes its own copy of the stored document as <folder>/<name>.kkproj and never
    // overwrites anything: a file the user chose to replace is moved aside, put back on failure.
    const QString unpackedProject
        = QDir(folder).absoluteFilePath(QFileInfo(container).completeBaseName() + QLatin1Char('.')
            + QLatin1String(appinfo::projectExtension));
    QString aside;
    if (QFileInfo::exists(unpackedProject)) {
        if (unpackedProject != target) {
            QMessageBox::warning(m_window, tr("Save Project As"),
                tr("%1 already exists. Choose another folder.").arg(native(unpackedProject)));
            return false;
        }
        aside = target + QStringLiteral(".replaced-%1").arg(QCoreApplication::applicationPid());
        if (!QFile::rename(target, aside)) {
            QMessageBox::critical(
                m_window, tr("Save Project As"), tr("Cannot replace %1.").arg(native(target)));
            return false;
        }
    }
    pack::UnpackResult result;
    runWithProgress(m_window, tr("Unpack Project"),
        tr("Writing the media of %1 into %2…").arg(QFileInfo(container).fileName(), native(folder)),
        [&](const TaskProgress& progress) {
            result = pack::unpackProject(container, folder, progress);
        });
    const auto restore = [&] {
        if (!aside.isEmpty())
            QFile::rename(aside, target);
    };
    if (result.status == pack::Status::Cancelled) {
        restore();
        return false;
    }
    if (result.status != pack::Status::Ok) {
        restore();
        QMessageBox::critical(m_window, tr("Unpack Project"),
            tr("The project could not be unpacked: %1").arg(result.error));
        return false;
    }
    // The open state (perhaps unsaved), its media pointing at the files just written.
    QString error;
    const QByteArray json = relinkToFolder(m_ctx.project->documentJson(target), container, folder);
    if (!writeFile(target, json, &error)) {
        restore();
        QMessageBox::critical(m_window, tr("Unpack Project"), error);
        return false;
    }
    if (result.projectFile != target)
        QFile::remove(result.projectFile);
    if (!aside.isEmpty())
        QFile::remove(aside);
    if (!m_ctx.project->load(target, &error)) {
        QMessageBox::critical(m_window, tr("Unpack Project"), error);
        return false;
    }
    releaseContainer();
    return true;
}

// ---- File menu: Pack, Unpack, Compact, Verify ----

void ProjectController::packProject()
{
    if (isSingleFile())
        return;
    // Pack reads a saved linked project: save first (a new project asks where).
    if (m_ctx.project->filePath().isEmpty() || m_ctx.project->isDirty()) {
        if (!save())
            return;
        if (isSingleFile())
            return; // Save As already made it a single-file project
    }
    const QString source = m_ctx.project->filePath();
    QString target = platform::getSaveFileName(m_window, tr("Pack Project"), singleFileFilter(),
        QLatin1String(kLastProjectDirKey),
        QFileInfo(source).completeBaseName() + QLatin1Char('.')
            + QLatin1String(pack::kContainerSuffix));
    if (target.isEmpty())
        return;
    if (!pack::isContainerFile(target))
        target += QLatin1Char('.') + QLatin1String(pack::kContainerSuffix);
    target = QFileInfo(target).absoluteFilePath();
    if (ProjectLock::isLocked(target)) {
        QMessageBox::warning(m_window, tr("Pack Project"),
            tr("%1 is open in another window or program.").arg(native(target)));
        return;
    }
    pack::PackResult result;
    runWithProgress(m_window, tr("Pack Project"),
        tr("Copying the project and its media into %1…").arg(QFileInfo(target).fileName()),
        [&](const TaskProgress& progress) {
            result = pack::packProject(source, target, progress);
        });
    if (result.status == pack::Status::Cancelled)
        return;
    if (result.status != pack::Status::Ok) {
        QMessageBox::critical(m_window, tr("Pack Project"),
            tr("The project could not be packed: %1").arg(result.error));
        return;
    }
    QMessageBox box(QMessageBox::Information, tr("Pack Project"),
        tr("%n media file(s) were copied into %1 (%2).", nullptr, result.embedded)
            .arg(native(target), sizeText(QFileInfo(target).size())),
        QMessageBox::NoButton, m_window);
    if (!result.skipped.isEmpty()) {
        box.setInformativeText(tr("%n media file(s) stay linked (not stored in the file):", nullptr,
                                   int(result.skipped.size()))
            + QLatin1Char('\n') + shortList(result.skipped));
        box.setDetailedText(result.skipped.join(QLatin1Char('\n')));
    }
    QPushButton* open = box.addButton(tr("&Open Packed Project"), QMessageBox::AcceptRole);
    box.addButton(tr("&Close"), QMessageBox::RejectRole);
    box.setDefaultButton(open);
    box.exec();
    if (box.clickedButton() == open && openFile(target, true)) {
        // openFile() remembered it as a recent project
    } else {
        addRecentProject(target);
        m_ctx.actions->action(QStringLiteral("file.pack_project"))->setEnabled(!isSingleFile());
    }
}

void ProjectController::unpackProject()
{
    if (!isSingleFile() || !m_package)
        return;
    const QString folder = platform::getExistingDirectory(m_window,
        tr("Unpack Project: Choose the Folder for the Media"), QLatin1String(kLastProjectDirKey));
    if (folder.isEmpty())
        return;
    const QString target
        = QDir(folder).absoluteFilePath(QFileInfo(m_package->path()).completeBaseName()
            + QLatin1Char('.') + QLatin1String(appinfo::projectExtension));
    if (QFileInfo::exists(target)) {
        QMessageBox::warning(m_window, tr("Unpack Project"),
            tr("%1 already exists. Choose another folder.").arg(native(target)));
        return;
    }
    const int media = int(m_package->media().size());
    if (!saveAsLinked(target))
        return;
    addRecentProject(target);
    notify(m_window, tr("Unpack Project"),
        tr("%n media file(s) were written to %1. The project is now the linked project %2.",
            nullptr, media)
            .arg(native(QDir(folder).absoluteFilePath(QLatin1String("media"))), native(target)));
}

void ProjectController::compactProject()
{
    if (!m_package || m_readOnly)
        return;
    // Compact keeps what the saved document uses: unsaved imports must be saved first.
    QString error;
    if (m_ctx.project->isDirty() && !saveContainer(&error)) {
        QMessageBox::critical(m_window, tr("Compact Project"), error);
        return;
    }
    const QString path = m_package->path();
    const qint64 before = QFileInfo(path).size();
    const qint64 estimate = m_package->compactedSizeEstimate();
    QStringList unused;
    for (const QString& entry : m_package->unusedMedia())
        unused.append(entry.section(QLatin1Char('/'), -1));
    QMessageBox ask(QMessageBox::Question, tr("Compact Project"),
        tr("Compacting rewrites “%1” without the media no clip of the media pool uses and "
           "without the older saved versions.\n\nCurrent size: %2\nEstimated size after "
           "compacting: %3")
            .arg(QFileInfo(path).fileName(), sizeText(before), sizeText(estimate)),
        QMessageBox::NoButton, m_window);
    ask.setObjectName(QStringLiteral("CompactQuestion"));
    if (unused.isEmpty()) {
        ask.setInformativeText(tr("No unused media."));
    } else {
        ask.setInformativeText(
            tr("%n unused media file(s) will be removed:", nullptr, int(unused.size()))
            + QLatin1Char('\n') + shortList(unused));
        ask.setDetailedText(unused.join(QLatin1Char('\n')));
    }
    QPushButton* compact = ask.addButton(tr("&Compact"), QMessageBox::AcceptRole);
    ask.addButton(QMessageBox::Cancel);
    ask.setDefaultButton(compact);
    ask.exec();
    if (ask.clickedButton() != compact)
        return;

    // Every reader of the file lets go before it is replaced (Windows cannot rename over an
    // open file); decoders open the new file afterwards.
    if (m_ctx.playback)
        m_ctx.playback->stop();
    QStringList embedded;
    const QString prefix = path + QLatin1Char('/');
    for (const MediaItem& m : m_ctx.project->mediaPool()->state().items) {
        if (m.path.startsWith(prefix))
            embedded.append(m.path);
    }
    media::MediaCache& cache = media::MediaCache::instance();
    for (const QString& p : std::as_const(embedded))
        cache.invalidate(p);
    cache.clearMemoryCache(); // idle preview decoders
    m_package.reset(); // the lock stays: nobody else writes meanwhile

    pack::CompactResult result;
    runWithProgress(m_window, tr("Compact Project"),
        tr("Compacting %1…").arg(QFileInfo(path).fileName()),
        [&](const TaskProgress& progress) { result = pack::compactProject(path, progress); });
    pack::invalidateIndexCache(path);
    m_package = pack::ProjectPackage::open(path, true, &error);
    if (!m_package) {
        releaseContainer();
        m_readOnly = true;
        updatePackageActions();
        QMessageBox::critical(m_window, tr("Compact Project"),
            tr("The project file could not be opened again: %1").arg(error));
        return;
    }
    for (const QString& p : std::as_const(embedded))
        cache.invalidate(p);
    m_ctx.project->mediaPool()->recheckOffline();
    if (result.status == pack::Status::Cancelled)
        return;
    if (result.status != pack::Status::Ok) {
        QMessageBox::critical(m_window, tr("Compact Project"),
            tr("The project could not be compacted: %1").arg(result.error));
        return;
    }
    notify(m_window, tr("Compact Project"),
        tr("“%1” was compacted from %2 to %3.")
                .arg(QFileInfo(path).fileName(), sizeText(result.sizeBefore),
                    sizeText(result.sizeAfter))
            + (result.removedMedia.isEmpty() ? QString()
                                             : QLatin1Char(' ')
                        + tr("%n unused media file(s) removed.", nullptr,
                            int(result.removedMedia.size()))));
}

void ProjectController::verifyProject()
{
    if (!isSingleFile())
        return;
    const QString path = containerPath();
    pack::VerifyResult result;
    const bool finished = runWithProgress(m_window, tr("Verify Project File"),
        tr("Checking %1…").arg(QFileInfo(path).fileName()), [&](const TaskProgress& progress) {
            result = pack::verifyContainer(path, true, progress);
        });
    if (!finished)
        return;
    if (result.ok) {
        QString text
            = tr("No problems found in “%1”: %n entries, %2 checked.", nullptr, result.entries)
                  .arg(QFileInfo(path).fileName(), sizeText(result.checkedBytes));
        if (!result.notes.isEmpty())
            text += QLatin1Char('\n') + shortList(result.notes);
        notify(m_window, tr("Verify Project File"), text);
        return;
    }
    QMessageBox box(QMessageBox::Warning, tr("Verify Project File"),
        tr("“%1” has %n problem(s).", nullptr, int(result.problems.size()))
            .arg(QFileInfo(path).fileName()),
        QMessageBox::NoButton, m_window);
    box.setObjectName(QStringLiteral("VerifyProblems"));
    box.setInformativeText(shortList(result.problems));
    box.setDetailedText((result.problems + result.notes).join(QLatin1Char('\n')));
    QPushButton* repair = nullptr;
    if (m_package && !m_readOnly) {
        repair = box.addButton(tr("&Repair"), QMessageBox::AcceptRole);
        repair->setToolTip(tr("Cuts off what an interrupted save left behind and writes a "
                              "fresh index; the last complete save is kept."));
    }
    box.addButton(QMessageBox::Close);
    box.exec();
    if (!repair || box.clickedButton() != repair)
        return;
    m_package.reset(); // the container is written by repairContainer() alone
    QString error;
    const bool repaired = pack::repairContainer(path, &error);
    pack::invalidateIndexCache(path);
    QString reopenError;
    m_package = pack::ProjectPackage::open(path, true, &reopenError);
    if (!m_package) {
        releaseContainer();
        m_readOnly = true;
        updatePackageActions();
        error = reopenError;
    }
    const pack::VerifyResult again = pack::verifyContainer(path, false);
    if (!repaired || !again.ok) {
        QMessageBox::critical(m_window, tr("Repair Project File"),
            tr("“%1” could not be repaired: %2")
                .arg(QFileInfo(path).fileName(),
                    error.isEmpty() ? again.problems.join(QLatin1Char('\n')) : error));
        return;
    }
    notify(m_window, tr("Repair Project File"),
        tr("“%1” was repaired. Verify it again to check every byte.")
            .arg(QFileInfo(path).fileName()));
}

} // namespace kk
