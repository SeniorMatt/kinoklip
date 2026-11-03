// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel operations: bins (create, rename, move, delete with contents), smart bins,
// power bins (copies between project and power bins), clip colours, flags, usage commands.
// Project changes are one undo step each; power bin changes are saved at once (PowerBins).
#include "core/MediaPool.h"
#include "core/MediaUsage.h"
#include "core/Plural.h"
#include "core/PowerBins.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/AppContext.h"
#include "ui/common/FolderImport.h"
#include "ui/common/MediaImport.h"
#include "ui/mediapool/BinList.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/mediapool/SmartBinDialog.h"

#include <QMessageBox>
#include <QSet>
#include <QTimer>

namespace kk {

namespace {
using Filter = MediaPoolFilterModel;

/// A copy of @p item for another pool: same file, name, colour, flags and metadata; a new id.
MediaItem copyForPool(const MediaItem& item, const QString& binId)
{
    MediaItem copy = item;
    copy.id.clear();
    copy.binId = binId;
    copy.addedMs = 0; // now
    return copy;
}
} // namespace

void MediaPoolPanel::editItems(const QString& label, const QStringList& ids,
    const std::function<void(MediaPool*, const QString&)>& change)
{
    MediaPool* project = m_ctx.project->mediaPool();
    QStringList projectIds;
    for (const QString& id : ids) {
        MediaPool* pool = m_model->poolOf(id);
        if (pool == project)
            projectIds.append(id);
        else if (pool)
            change(pool, id); // power bins: saved at once, not undoable
    }
    if (projectIds.isEmpty())
        return;
    m_ctx.project->beginEdit(label);
    for (const QString& id : std::as_const(projectIds))
        change(project, id);
    m_ctx.project->endEdit();
}

// ---- Import ----

QStringList MediaPoolPanel::importFiles(const QStringList& paths, const QString& binKey)
{
    if (!Filter::isPowerKey(binKey)) {
        // Folders (dropped from a file manager) import with their subfolders as bins, like
        // Resolve's "Add Folder and SubFolders into Media Pool (Create Bins)"; files as before.
        QStringList files = paths;
        const QStringList folders = takeFolders(files);
        QStringList ids;
        if (!files.isEmpty())
            ids = importMediaWithFeedback(m_ctx, files, this, Filter::idOfKey(binKey));
        if (!folders.isEmpty())
            ids += importMediaFoldersWithFeedback(
                m_ctx, folders, FolderImportOptions {}, this, Filter::idOfKey(binKey));
        return ids;
    }
    if (!m_ctx.powerBins)
        return {};
    const ImportResult result
        = m_ctx.powerBins->pool()->importFiles(paths, Filter::idOfKey(binKey));
    if (!result.warnings.isEmpty() && isVisible())
        QMessageBox::warning(
            this, tr("Import into Power Bins"), result.warnings.join(QLatin1Char('\n')));
    return result.importedIds;
}

// ---- Bins ----

QString MediaPoolPanel::createBin(const QString& name, const QString& parentKey)
{
    QString key;
    if (Filter::isPowerKey(parentKey) && m_ctx.powerBins) {
        key = QLatin1String(Filter::kPowerPrefix)
            + m_ctx.powerBins->pool()->addBin(name, Filter::idOfKey(parentKey));
    } else {
        // Inside a smart bin there is no place for bins: the new bin goes into Master.
        const QString parent = Filter::isSmartKey(parentKey) ? QString() : parentKey;
        m_ctx.project->beginEdit(tr("New Bin"));
        key = m_ctx.project->mediaPool()->addBin(name, parent);
        m_ctx.project->endEdit();
    }
    // Start renaming the new bin right away (UI only).
    if (QTreeWidgetItem* item = m_bins->itemForKey(key)) {
        m_bins->scrollToItem(item);
        if (isVisible() && m_bins->isVisible())
            m_bins->editItem(item);
    }
    return key;
}

bool MediaPoolPanel::renameBin(const QString& binKey, const QString& name)
{
    MediaPool* pool = poolForKey(binKey);
    const QString id = Filter::idOfKey(binKey);
    const Bin* bin = pool && !id.isEmpty() ? pool->bin(id) : nullptr;
    const QString trimmed = name.trimmed();
    if (bin && bin->name == trimmed)
        return false; // unchanged
    if (!bin || trimmed.isEmpty()) {
        // Restore the shown name, from the event loop (called from the tree's itemChanged).
        QTimer::singleShot(0, this, &MediaPoolPanel::rebuildBins);
        return false;
    }
    if (pool != m_ctx.project->mediaPool())
        return pool->renameBin(id, trimmed);
    m_ctx.project->beginEdit(tr("Rename Bin"));
    const bool ok = pool->renameBin(id, trimmed);
    m_ctx.project->endEdit();
    return ok;
}

bool MediaPoolPanel::deleteBin(const QString& binKey, bool confirm)
{
    MediaPool* pool = poolForKey(binKey);
    const QString id = Filter::idOfKey(binKey);
    const Bin* bin = pool && !id.isEmpty() ? pool->bin(id) : nullptr;
    if (!bin)
        return false;
    int files = 0;
    int used = 0;
    for (const MediaItem& m : pool->items()) {
        if (m.isTimeline() || m.binId.isEmpty() || !pool->binIsInside(m.binId, id))
            continue;
        ++files;
        used += pool == m_ctx.project->mediaPool() ? m_model->usageCounts().value(m.id, 0) : 0;
    }
    if (confirm && files > 0) {
        QString text = plural(files, tr("Delete the bin \"%1\" with its bins and the clip in it?"),
            tr("Delete the bin \"%1\" with its bins and the %n clips in it?"))
                           .arg(bin->name);
        if (used > 0)
            text += QLatin1Char(' ')
                + plural(used, tr("%n timeline clip uses them and will show as offline."),
                    tr("%n timeline clips use them and will show as offline."));
        if (QMessageBox::question(this, tr("Delete Bin"), text) != QMessageBox::Yes)
            return false;
    }
    const bool undoable = pool == m_ctx.project->mediaPool();
    if (undoable)
        m_ctx.project->beginEdit(tr("Delete Bin"));
    pool->removeBinWithContents(id);
    if (undoable)
        m_ctx.project->endEdit();
    return !pool->bin(id);
}

bool MediaPoolPanel::moveBin(const QString& binKey, const QString& targetKey, int place)
{
    MediaPool* pool = poolForKey(binKey);
    if (!pool || pool != poolForKey(targetKey) || binKey == targetKey)
        return false;
    const QString id = Filter::idOfKey(binKey);
    const QString target = Filter::idOfKey(targetKey);
    QString parent = target;
    int index = -1;
    if (BinList::DropPlace(place) != BinList::DropPlace::Into) {
        const Bin* t = pool->bin(target);
        if (!t)
            return false;
        parent = t->parentId;
        QStringList siblings = pool->childBins(parent);
        siblings.removeAll(id);
        index = int(siblings.indexOf(target))
            + (BinList::DropPlace(place) == BinList::DropPlace::After ? 1 : 0);
    }
    if (pool->binIsInside(parent, id))
        return false; // into itself or one of its sub-bins
    if (pool != m_ctx.project->mediaPool())
        return pool->moveBin(id, parent, index);
    m_ctx.project->beginEdit(tr("Move Bin"));
    const bool ok = pool->moveBin(id, parent, index);
    m_ctx.project->endEdit();
    return ok;
}

int MediaPoolPanel::moveItemsToBin(const QStringList& ids, const QString& binKey)
{
    if (Filter::isSmartKey(binKey))
        return 0; // smart bins are not containers
    MediaPool* project = m_ctx.project->mediaPool();
    MediaPool* power = m_ctx.powerBins ? m_ctx.powerBins->pool() : nullptr;
    const bool toPower = Filter::isPowerKey(binKey);
    MediaPool* target = toPower ? power : project;
    const QString binId = Filter::idOfKey(binKey);
    if (!target || (!binId.isEmpty() && !target->bin(binId)))
        return 0;
    QStringList same;
    QStringList other;
    for (const QString& id : ids) {
        MediaPool* source = m_model->poolOf(id);
        if (source == target)
            same.append(id);
        else if (source)
            other.append(id);
    }
    int moved = 0;
    auto moveWithin = [&](MediaPool* pool) {
        for (const QString& id : std::as_const(same)) {
            const MediaItem* item = pool->item(id);
            if (item && item->binId != binId)
                moved += pool->moveItemToBin(id, binId) ? 1 : 0;
        }
    };
    if (toPower) {
        moveWithin(power);
        // Project media into a power bin: a copy (the project keeps its item; timelines and
        // compound clips belong to the project and are not copied).
        for (const QString& id : std::as_const(other)) {
            const MediaItem* item = project->item(id);
            if (!item || item->kind != MediaItemKind::File)
                continue;
            if (const MediaItem* existing = power->itemByPath(item->path)) {
                moved += power->moveItemToBin(existing->id, binId) ? 1 : 0;
                continue;
            }
            power->addItem(copyForPool(*item, binId));
            ++moved;
        }
        return moved;
    }
    m_ctx.project->beginEdit(tr("Move to Bin"));
    moveWithin(project);
    moved += int(ensureInProject(other, binId).size());
    m_ctx.project->endEdit();
    return moved;
}

int MediaPoolPanel::deleteItems(const QStringList& ids)
{
    if (ids.isEmpty())
        return 0;
    // Timeline / compound clip items delete their timeline (Project, one step each; the last
    // timeline of a project stays); media items go in one step; power bin items at once.
    QStringList files;
    QVector<quint64> timelines;
    int removed = 0;
    for (const QString& id : ids) {
        MediaPool* pool = m_model->poolOf(id);
        if (!pool)
            continue;
        if (pool != m_ctx.project->mediaPool()) {
            removed += pool->removeItem(id) ? 1 : 0;
            continue;
        }
        const MediaItem* item = pool->item(id);
        if (item && item->isTimeline())
            timelines.append(item->timelineUid);
        else
            files.append(id);
    }
    if (!files.isEmpty()) {
        m_ctx.project->beginEdit(
            files.size() == 1 ? tr("Delete Media") : tr("Delete %1 Media Items").arg(files.size()));
        for (const QString& id : std::as_const(files))
            removed += m_ctx.project->mediaPool()->removeItem(id) ? 1 : 0;
        m_ctx.project->endEdit();
    }
    for (const quint64 uid : std::as_const(timelines))
        removed += m_ctx.project->deleteTimeline(uid) ? 1 : 0;
    return removed;
}

// ---- Smart bins ----

QString MediaPoolPanel::createSmartBin(const SmartBin& bin)
{
    m_ctx.project->beginEdit(tr("New Smart Bin"));
    const QString id = m_ctx.project->mediaPool()->addSmartBin(bin);
    m_ctx.project->endEdit();
    return QLatin1String(Filter::kSmartPrefix) + id;
}

bool MediaPoolPanel::updateSmartBin(const SmartBin& bin)
{
    m_ctx.project->beginEdit(tr("Edit Smart Bin"));
    const bool ok = m_ctx.project->mediaPool()->updateSmartBin(bin);
    m_ctx.project->endEdit();
    return ok;
}

bool MediaPoolPanel::deleteSmartBin(const QString& key)
{
    m_ctx.project->beginEdit(tr("Delete Smart Bin"));
    const bool ok = m_ctx.project->mediaPool()->removeSmartBin(Filter::idOfKey(key));
    m_ctx.project->endEdit();
    return ok;
}

QString MediaPoolPanel::newSmartBinWithDialog()
{
    SmartBinDialog dialog(SmartBin {}, this);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    const QString key = createSmartBin(dialog.smartBin());
    setCurrentBinKey(key);
    return key;
}

void MediaPoolPanel::editSmartBinWithDialog(const QString& key)
{
    const SmartBin* bin = m_ctx.project->mediaPool()->smartBin(Filter::idOfKey(key));
    if (!bin)
        return;
    SmartBinDialog dialog(*bin, this);
    if (dialog.exec() == QDialog::Accepted)
        updateSmartBin(dialog.smartBin());
}

// ---- Clip colour and flags ----

void MediaPoolPanel::setClipColor(const QStringList& ids, const QString& colorId)
{
    editItems(colorId.isEmpty() ? tr("Clear Clip Color") : tr("Set Clip Color"), ids,
        [&colorId](MediaPool* pool, const QString& id) { pool->setClipColor(id, colorId); });
}

void MediaPoolPanel::setFlag(const QStringList& ids, int flagIndex, bool on)
{
    if (flagIndex < 0 || flagIndex >= 32)
        return;
    const quint32 bit = 1u << flagIndex;
    editItems(on ? tr("Add Flag") : tr("Remove Flag"), ids,
        [bit, on](MediaPool* pool, const QString& id) {
            if (const MediaItem* item = pool->item(id))
                pool->setFlags(id, on ? (item->flags | bit) : (item->flags & ~bit));
        });
}

void MediaPoolPanel::clearFlags(const QStringList& ids)
{
    editItems(tr("Clear All Flags"), ids,
        [](MediaPool* pool, const QString& id) { pool->setFlags(id, 0); });
}

// ---- Power bins -> project, usage ----

QStringList MediaPoolPanel::ensureInProject(const QStringList& ids, const QString& binId)
{
    MediaPool* project = m_ctx.project->mediaPool();
    MediaPool* power = m_ctx.powerBins ? m_ctx.powerBins->pool() : nullptr;
    QStringList out;
    bool opened = false;
    for (const QString& id : ids) {
        if (project->item(id)) {
            out.append(id);
            continue;
        }
        const MediaItem* item = power ? power->item(id) : nullptr;
        if (!item)
            continue;
        // The same file already in the project: use it (moved when a bin was asked for).
        if (const MediaItem* existing = project->itemByPath(item->path)) {
            if (!binId.isEmpty() && existing->binId != binId) {
                if (!opened) {
                    m_ctx.project->beginEdit(tr("Add Power Bin Media"));
                    opened = true;
                }
                project->moveItemToBin(existing->id, binId);
            }
            out.append(existing->id);
            continue;
        }
        if (!opened) {
            m_ctx.project->beginEdit(tr("Add Power Bin Media"));
            opened = true;
        }
        out.append(project->addItem(copyForPool(*item, binId)));
    }
    if (opened)
        m_ctx.project->endEdit();
    return out;
}

int MediaPoolPanel::selectClipsInTimeline(const QStringList& ids)
{
    if (!m_ctx.selection)
        return 0;
    const QSet<QString> set(ids.cbegin(), ids.cend());
    const QVector<quint64> clips = usage::clipsUsing(
        m_ctx.project->timeline()->data(), set, m_ctx.project->mediaPool()->state());
    m_ctx.selection->setClips(clips);
    return int(clips.size());
}

QStringList MediaPoolPanel::unusedClips() const
{
    m_model->refreshUsage(); // exact even right after an edit (the model recounts lazily)
    return usage::unusedFileItems(m_ctx.project->mediaPool()->state(), m_model->usageCounts());
}

int MediaPoolPanel::removeUnusedClips(bool confirm)
{
    const QStringList unused = unusedClips();
    if (unused.isEmpty()) {
        if (confirm)
            QMessageBox::information(this, tr("Remove Unused Clips"),
                tr("Every clip of the media pool is used in a timeline."));
        return 0;
    }
    if (confirm
        && QMessageBox::question(this, tr("Remove Unused Clips"),
               plural(unused.size(),
                   tr("Remove %n clip that is not used in any timeline from the media pool? "
                      "The file stays on disk."),
                   tr("Remove %n clips that are not used in any timeline from the media pool? "
                      "The files stay on disk.")))
            != QMessageBox::Yes)
        return 0;
    m_ctx.project->beginEdit(tr("Remove Unused Clips"));
    int removed = 0;
    for (const QString& id : unused)
        removed += m_ctx.project->mediaPool()->removeItem(id) ? 1 : 0;
    m_ctx.project->endEdit();
    return removed;
}

} // namespace kk
