// SPDX-License-Identifier: GPL-3.0-or-later
// FolderImport implementation: scan + probe on a worker thread, store (single-file projects),
// then one undoable edit that creates the bins and adds the items.
#include "ui/common/FolderImport.h"

#include "core/MediaPool.h"
#include "core/Plural.h"
#include "core/Project.h"
#include "media/FolderScan.h"
#include "media/SupportedFormats.h"
#include "platform/FileDialogs.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaImport.h"
#include "ui/common/MediaStore.h"
#include "ui/common/ProgressTask.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QVBoxLayout>

#include <vector>

namespace kk {

Q_LOGGING_CATEGORY(lcFolderImport, "kk.ui.folderimport", QtInfoMsg)

namespace {

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("kk::FolderImport", text, disambiguation, n);
}

constexpr int kMaxListedFailures = 12; // keep the summary a sensible size
constexpr char kSubfoldersKey[] = "import/folderIncludeSubfolders";
constexpr char kCreateBinsKey[] = "import/folderCreateBins";
/// Same remembered folder as File > Import Media (src/app/ProjectController.cpp).
constexpr char kLastMediaDirKey[] = "paths/lastMediaDir";

std::function<bool(FolderImportOptions*)>& optionsHandler()
{
    static std::function<bool(FolderImportOptions*)> handler;
    return handler;
}

/// The extensions File > Import Media offers (mediaFileNamePatterns(), which also has the
/// subtitle formats) plus every extension the media engine registers (HEIF, EXR, camera RAW
/// ...): the folder scan is a pre-filter, probe() still decides.
const QSet<QString>& importableExtensions()
{
    static const QSet<QString> set = [] {
        QSet<QString> s;
        for (const QString& pattern : mediaFileNamePatterns())
            s.insert(pattern.mid(2).toLower()); // "*.mp4" -> "mp4"
        for (const QString& ext : media::SupportedFormats::allExtensions())
            s.insert(ext);
        return s;
    }();
    return set;
}

/// A folder of the scan, flattened: the bin to create for it and where.
struct FlatFolder
{
    QString name;
    int parent = -1; ///< index into the flat list; -1 = directly in the target bin
};

/// A file to import and the folder it came from.
struct Candidate
{
    QString path;
    int folder = -1;
    PreparedMedia prepared;
};

void flatten(const media::ScannedFolder& node, int parent, std::vector<FlatFolder>& folders,
    std::vector<Candidate>& files, QSet<QString>& seen, const QSet<QString>& inPool,
    int* duplicates)
{
    const int index = int(folders.size());
    folders.push_back({node.name, parent});
    for (const QString& path : node.files) {
        if (inPool.contains(path) || seen.contains(path)) {
            ++*duplicates;
            continue;
        }
        seen.insert(path);
        files.push_back({path, index, {}});
    }
    for (const media::ScannedFolder& sub : node.subfolders)
        flatten(sub, index, folders, files, seen, inPool, duplicates);
}

QSet<QString> poolPaths(const MediaPool& pool)
{
    QSet<QString> paths;
    for (const MediaItem& m : pool.state().items) {
        if (!m.isTimeline() && !m.path.isEmpty())
            paths.insert(m.path);
    }
    return paths;
}

} // namespace

QStringList takeFolders(QStringList& paths)
{
    QStringList folders;
    for (auto it = paths.begin(); it != paths.end();) {
        if (QFileInfo(*it).isDir()) {
            folders.append(*it);
            it = paths.erase(it);
        } else {
            ++it;
        }
    }
    return folders;
}

FolderImportResult importMediaFolders(AppContext& ctx, const QStringList& folders,
    const FolderImportOptions& options, QWidget* parent, const QString& binId)
{
    FolderImportResult result;
    if (!ctx.project || folders.isEmpty())
        return result;
    MediaPool* pool = ctx.project->mediaPool();
    const QString targetBin = binId.isEmpty() || pool->bin(binId) ? binId : QString();

    media::FolderScanOptions scan;
    scan.includeSubfolders = options.includeSubfolders;
    scan.extensions = importableExtensions();
    scan.sequenceFrameRate = ctx.project->settings().fps;

    // ---- Worker: scan every folder, then probe every new file ----
    // The worker only reads the file system and its own copies (folders, the pool's paths);
    // the pool itself is changed below on the GUI thread.
    const QSet<QString> inPool = poolPaths(*pool);
    std::vector<FlatFolder> flat;
    std::vector<Candidate> files;
    bool scanCancelled = false;
    const bool finished = runWithProgress(parent, tr("Import Media Folder"),
        tr("Reading the folder…"), [&](const TaskProgress& progress) {
            QSet<QString> seen;
            for (const QString& folder : folders) {
                const media::FolderScanResult scanned
                    = media::scanMediaFolder(folder, scan, [&](int) { return progress(0, 0); });
                if (scanned.cancelled) {
                    scanCancelled = true;
                    return;
                }
                if (!scanned.ok) {
                    result.failures.append(tr("%1: cannot read the folder").arg(folder));
                    continue;
                }
                result.skippedHidden += scanned.skippedHidden;
                result.skippedUnsupported += scanned.skippedUnsupported;
                result.skippedLinks += scanned.skippedLinks;
                flatten(scanned.root, -1, flat, files, seen, inPool, &result.duplicates);
            }
            const qint64 total = qint64(files.size());
            for (qsizetype i = 0; i < qsizetype(files.size()); ++i) {
                if (!progress(i, total)) {
                    scanCancelled = true;
                    return;
                }
                files[size_t(i)].prepared = MediaPool::prepareFile(files[size_t(i)].path);
            }
            progress(total, total);
        });
    if (!finished || scanCancelled) {
        result = {};
        result.cancelled = true;
        return result;
    }

    std::vector<Candidate*> ok;
    for (Candidate& c : files) {
        if (c.prepared.ok)
            ok.push_back(&c);
        else
            result.failures.append(tr("%1: %2").arg(c.path, c.prepared.error));
    }

    // ---- Single-file project: copy the files in (the shared Import Media path) ----
    // Only files that probed fine are copied, so unsupported files never enter the project.
    if (!ok.empty() && ctx.mediaStore && ctx.mediaStore->storesMedia()) {
        QStringList paths;
        paths.reserve(qsizetype(ok.size()));
        for (const Candidate* c : ok)
            paths.append(c->path);
        if (!ctx.mediaStore->storeMedia(paths, parent)) {
            const FolderImportResult none;
            result = none;
            result.cancelled = true;
            return result;
        }
        // The probe ran on the original; the stored copy has the same content.
        for (size_t i = 0; i < ok.size() && qsizetype(i) < paths.size(); ++i) {
            MediaItem& item = ok[i]->prepared.item;
            if (paths[qsizetype(i)] == item.path)
                continue;
            item.path = paths[qsizetype(i)];
            item.info.path = item.path;
            item.fileModifiedMs = MediaPool::modifiedMsOf(item.path);
        }
        // Content stored before comes back as the existing copy: a duplicate now.
        const QSet<QString> stored = poolPaths(*pool);
        QSet<QString> seen;
        std::vector<Candidate*> fresh;
        for (Candidate* c : ok) {
            const QString& path = c->prepared.item.path;
            if (stored.contains(path) || seen.contains(path)) {
                ++result.duplicates;
                continue;
            }
            seen.insert(path);
            fresh.push_back(c);
        }
        ok = std::move(fresh);
    }
    if (ok.empty())
        return result;

    // ---- One undo step: the bins (only for folders that get items) and the items ----
    std::vector<QString> binOf(flat.size()); // created bin per flat folder, empty = not yet
    const std::function<QString(int)> binFor = [&](int folder) -> QString {
        if (folder < 0 || !options.createBins)
            return targetBin;
        QString& id = binOf[size_t(folder)];
        if (id.isEmpty()) {
            const QString parentBin = binFor(flat[size_t(folder)].parent);
            id = pool->addBin(flat[size_t(folder)].name, parentBin);
            result.createdBinIds.append(id);
        }
        return id;
    };
    ctx.project->beginEdit(tr("Import Media Folder"));
    for (Candidate* c : ok) {
        if (!c->prepared.note.isEmpty())
            result.notes.append(tr("%1: %2").arg(c->path, c->prepared.note));
        MediaItem item = std::move(c->prepared.item);
        item.binId = binFor(c->folder);
        result.importedIds.append(pool->addItem(std::move(item)));
    }
    ctx.project->endEdit();
    qCInfo(lcFolderImport) << "folder import:" << result.importedIds.size() << "items,"
                           << result.createdBinIds.size() << "bins," << result.duplicates
                           << "duplicates," << result.failures.size() << "failed,"
                           << result.skippedUnsupported << "unsupported," << result.skippedHidden
                           << "hidden," << result.skippedLinks << "links";
    return result;
}

QStringList importMediaFoldersWithFeedback(AppContext& ctx, const QStringList& folders,
    const FolderImportOptions& options, QWidget* parent, const QString& binId)
{
    const FolderImportResult r = importMediaFolders(ctx, folders, options, parent, binId);
    if (r.cancelled)
        return {};
    if (r.failures.isEmpty() && r.duplicates == 0 && r.notes.isEmpty() && !r.importedIds.isEmpty())
        return r.importedIds;
    // One summary for the whole import instead of a dialog per file.
    QStringList lines;
    if (r.duplicates > 0)
        lines.append(plural(r.duplicates, tr("%n file already in the media pool was skipped."),
            tr("%n files already in the media pool were skipped.")));
    if (r.skippedUnsupported > 0)
        lines.append(plural(r.skippedUnsupported, tr("%n file of another type was skipped."),
            tr("%n files of other types were skipped.")));
    if (r.skippedHidden > 0)
        lines.append(plural(r.skippedHidden, tr("%n hidden or system file was skipped."),
            tr("%n hidden or system files were skipped.")));
    if (!r.failures.isEmpty()) {
        lines.append(plural(r.failures.size(), tr("%n file could not be imported:"),
            tr("%n files could not be imported:")));
        lines += r.failures.mid(0, kMaxListedFailures);
        if (r.failures.size() > kMaxListedFailures)
            lines.append(tr("… and %1 more").arg(r.failures.size() - kMaxListedFailures));
    }
    lines += r.notes;
    const QString text = r.importedIds.isEmpty()
        ? tr("No new media files were imported.")
        : plural(r.importedIds.size(), tr("%n file imported."), tr("%n files imported."));
    QMessageBox box(r.failures.isEmpty() ? QMessageBox::Information : QMessageBox::Warning,
        tr("Import Media Folder"), text, QMessageBox::Ok, parent);
    box.setInformativeText(lines.join(QLatin1Char('\n')));
    if (!r.failures.isEmpty())
        box.setDetailedText(r.failures.join(QLatin1Char('\n')));
    box.exec();
    return r.importedIds;
}

bool askFolderImportOptions(QWidget* parent, const QString& folder, FolderImportOptions* options)
{
    QSettings settings;
    FolderImportOptions chosen;
    chosen.includeSubfolders = settings.value(QLatin1String(kSubfoldersKey), true).toBool();
    chosen.createBins = settings.value(QLatin1String(kCreateBinsKey), true).toBool();
    if (optionsHandler()) {
        if (!optionsHandler()(&chosen))
            return false;
    } else {
        QDialog dialog(parent);
        dialog.setWindowTitle(tr("Import Media Folder"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* label = new QLabel(tr("Import the media files of “%1”.")
                                     .arg(QDir::toNativeSeparators(QFileInfo(folder).fileName())));
        label->setToolTip(QDir::toNativeSeparators(folder));
        layout->addWidget(label);
        auto* subfolders = new QCheckBox(tr("Include subfolders"));
        subfolders->setChecked(chosen.includeSubfolders);
        layout->addWidget(subfolders);
        auto* bins = new QCheckBox(tr("Create bins for subfolders"));
        bins->setToolTip(tr("A bin named after the folder, with a sub-bin for each subfolder"));
        bins->setChecked(chosen.createBins);
        layout->addWidget(bins);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Import"));
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return false;
        chosen.includeSubfolders = subfolders->isChecked();
        chosen.createBins = bins->isChecked();
    }
    settings.setValue(QLatin1String(kSubfoldersKey), chosen.includeSubfolders);
    settings.setValue(QLatin1String(kCreateBinsKey), chosen.createBins);
    *options = chosen;
    return true;
}

void setFolderImportOptionsHandlerForTests(std::function<bool(FolderImportOptions*)> handler)
{
    optionsHandler() = std::move(handler);
}

QStringList importMediaFolderWithDialogs(AppContext& ctx, QWidget* parent, const QString& binId)
{
    const QString folder = platform::getExistingDirectory(
        parent, tr("Import Media Folder"), QLatin1String(kLastMediaDirKey));
    if (folder.isEmpty())
        return {};
    FolderImportOptions options;
    if (!askFolderImportOptions(parent, folder, &options))
        return {};
    return importMediaFoldersWithFeedback(ctx, {folder}, options, parent, binId);
}

} // namespace kk
