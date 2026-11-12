// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel context menus: items (clip colour, flags, usage commands, proxies), bins,
// list header (columns), clip colour filter; plus the dialogs they open.
#include "core/ClipColors.h"
#include "core/MediaPool.h"
#include "core/Plural.h"
#include "core/PowerBins.h"
#include "core/Project.h"
#include "media/MediaSource.h"
#include "pack/EmbeddedMedia.h"
#include "platform/FileDialogs.h"
#include "platform/FileManager.h"
#include "platform/OpenUrl.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/FolderImport.h"
#include "ui/common/Icons.h"
#include "ui/common/MediaImport.h"
#include "ui/common/MediaStore.h"
#include "ui/common/MenuLayouts.h"
#include "ui/common/SaveMediaCopy.h"
#include "ui/dialogs/ClipAttributesDialog.h"
#include "ui/mediapool/BinList.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineDialogs.h"

#include <QAbstractItemView>
#include <QFileInfo>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QTreeView>
#include <QUrl>

#include <algorithm>

namespace kk {

namespace {

constexpr int kSwatchSize = 12; ///< px, colour squares in menus

/// A small filled square for colour menus.
QIcon swatch(const QColor& color)
{
    QPixmap pm(kSwatchSize, kSwatchSize);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setPen(color.darker(160));
    p.setBrush(color);
    p.drawRect(0, 0, kSwatchSize - 1, kSwatchSize - 1);
    return QIcon(pm);
}

/// Adds the command @p id of the registry to @p menu (if registered).
void addRegistered(QMenu& menu, ActionRegistry* reg, const char* id)
{
    if (reg && reg->contains(QLatin1String(id)))
        menu.addAction(reg->action(QLatin1String(id)));
}

} // namespace

void MediaPoolPanel::addColorMenu(QMenu& menu, const QStringList& ids)
{
    QMenu* sub = menu.addMenu(tr("Clip Color"));
    sub->setObjectName(QStringLiteral("ClipColorMenu"));
    // The checked entry: the colour shared by the whole selection, if any.
    QString shared;
    bool same = true;
    for (const QString& id : ids) {
        const MediaItem* item = m_model->item(id);
        const QString c = item ? item->clipColor : QString();
        if (&id == &ids.front())
            shared = c;
        else if (c != shared)
            same = false;
    }
    int shown = 0;
    for (const PaletteColor& c : clipColorPalette()) {
        const QString colorId = QLatin1String(c.id);
        if (menulayout::paletteBreakBefore(shown++))
            sub->addSeparator();
        QAction* a = sub->addAction(swatch(QColor(c.rgb)), clipColorName(colorId), this,
            [this, ids, colorId] { setClipColor(ids, colorId); });
        a->setCheckable(true);
        a->setChecked(same && shared == colorId);
    }
    sub->addSeparator();
    sub->addAction(tr("Clear Color"), this, [this, ids] { setClipColor(ids, QString()); });
}

void MediaPoolPanel::addFlagMenu(QMenu& menu, const QStringList& ids)
{
    QMenu* sub = menu.addMenu(icon(QStringLiteral("flag")), tr("Flags"));
    sub->setObjectName(QStringLiteral("FlagMenu"));
    quint32 all = ~0u; // flags every selected item has
    for (const QString& id : ids) {
        const MediaItem* item = m_model->item(id);
        all &= item ? item->flags : 0u;
    }
    for (int i = 0; i < int(flagPalette().size()); ++i) {
        const bool on = (all & (1u << i)) != 0;
        if (menulayout::paletteBreakBefore(i))
            sub->addSeparator();
        QAction* a = sub->addAction(
            swatch(flagColor(i)), flagName(i), this, [this, ids, i, on] { setFlag(ids, i, !on); });
        a->setCheckable(true);
        a->setChecked(on);
    }
    sub->addSeparator();
    sub->addAction(tr("Clear All Flags"), this, [this, ids] { clearFlags(ids); });
}

void MediaPoolPanel::fillColorFilterMenu(QMenu* menu)
{
    menu->clear();
    const QString current = m_proxy->colorFilter();
    auto add = [this, menu, &current](const QIcon& ic, const QString& text, const QString& id) {
        QAction* a = menu->addAction(ic, text, this, [this, id] { setColorFilter(id); });
        a->setCheckable(true);
        a->setChecked(current == id);
    };
    add({}, tr("All Clips"), QString());
    add({}, tr("Clips without Color"), QStringLiteral("none"));
    menu->addSeparator();
    for (const PaletteColor& c : clipColorPalette())
        add(swatch(QColor(c.rgb)), clipColorName(QLatin1String(c.id)), QLatin1String(c.id));
}

void MediaPoolPanel::showHeaderMenu(const QPoint& globalPos)
{
    // Column chooser (Resolve: right-click the list header).
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("ColumnMenu"));
    for (int c = MediaPoolModel::NameColumn + 1; c < MediaPoolModel::ColumnCount; ++c) {
        QAction* a
            = menu.addAction(m_model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString(),
                this, [this, c](bool on) { setColumnVisible(c, on); });
        a->setCheckable(true);
        a->setChecked(isColumnVisible(c));
    }
    menu.addSeparator();
    menu.addAction(tr("Reset Columns"), this, [this] {
        for (int c = MediaPoolModel::NameColumn + 1; c < MediaPoolModel::ColumnCount; ++c)
            setColumnVisible(c, MediaPoolModel::columnVisibleByDefault(c));
    });
    menu.exec(globalPos);
}

void MediaPoolPanel::importWithDialog()
{
    // Same remembered folder as File > Import Media (src/app/ProjectController.cpp).
    const QStringList paths = platform::getOpenFileNames(
        this, tr("Import Media"), mediaFileDialogFilter(), QStringLiteral("paths/lastMediaDir"));
    if (!paths.isEmpty())
        importFiles(paths, importTargetKey());
}

void MediaPoolPanel::importFolderWithDialog(const QString& binKey)
{
    const QString target = MediaPoolFilterModel::idOfKey(binKey);
    showImportedItems(importMediaFolderWithDialogs(m_ctx, this, target), target);
}

void MediaPoolPanel::renameCurrentItem()
{
    const QModelIndex current = m_selection->currentIndex();
    if (current.isValid())
        currentView()->edit(current.siblingAtColumn(MediaPoolModel::NameColumn));
}

void MediaPoolPanel::confirmAndDelete(const QStringList& ids)
{
    if (ids.isEmpty())
        return;
    // Count timeline clips that would go offline, so the user knows what happens.
    QSet<QString> projectIds;
    for (const QString& id : ids) {
        if (m_ctx.project->mediaPool()->item(id))
            projectIds.insert(id);
    }
    int used = 0;
    for (const QString& id : std::as_const(projectIds))
        used += m_model->usageCounts().value(id, 0);
    if (used > 0) {
        const auto answer = QMessageBox::question(this, tr("Delete Media"),
            plural(used,
                tr("%n clip on the timeline uses this media and will show as offline. "
                   "Delete anyway?"),
                tr("%n clips on the timeline use this media and will show as offline. "
                   "Delete anyway?")));
        if (answer != QMessageBox::Yes)
            return;
    }
    deleteItems(ids);
}

void MediaPoolPanel::revealInFileManager(const QString& id)
{
    const MediaItem* item = m_model->item(id);
    if (!item)
        return;
    // Media stored inside a single-file project have no file of their own: the project file
    // is shown (and selected) instead.
    QString container;
    QString entry;
    if (pack::mayBeEmbeddedPath(item->path)
        && pack::splitEmbeddedPath(item->path, &container, &entry)) {
        platform::showInFileManager(container);
        return;
    }
    // Opening the containing folder works on every desktop; selecting the file itself would
    // need the platform-specific FileManager1 D-Bus interface (TODO(panels): src/platform).
    // platform::openUrl: the file manager must start with the user's environment, not the
    // bundle's library/plugin paths.
    platform::openUrl(QUrl::fromLocalFile(QFileInfo(item->path).absolutePath()));
}

void MediaPoolPanel::addFileRepairCommands(
    QMenu& menu, const QStringList& ids, const QString& current, bool power)
{
    const MediaItem* currentItem = m_model->item(current);
    // A single-file project: linked files of the selection can be copied in.
    if (!power && m_ctx.mediaStore && m_ctx.mediaStore->storesMedia()) {
        const QStringList targets = ids.contains(current) ? ids : QStringList {current};
        const bool anyLinked
            = std::any_of(targets.cbegin(), targets.cend(), [this](const QString& id) {
                  const MediaItem* m = m_model->item(id);
                  return m && !m->isTimeline() && isLinkedFile(m->path);
              });
        if (anyLinked) {
            menu.addAction(tr("Store in Project File"), this,
                [this, targets] { storeItemsInProject(m_ctx, targets, this); });
        }
    }
    // Media stored in the project file can be saved out to the computer again.
    if (!power && m_ctx.project
        && !storedCopySources(
            *m_ctx.project->mediaPool(), ids.contains(current) ? ids : QStringList {current})
                .isEmpty()) {
        QStringList targets = ids.contains(current) ? ids : QStringList {current};
        menu.addAction(tr("Save a Copy to Computer…"), this,
            [this, targets] { saveCopiesToComputer(m_ctx, targets, this); });
    }
    // Resolve's "Relink Selected Clips": offered when one of the selected items is offline.
    const QStringList selectedIds = ids.contains(current) ? ids : QStringList {current};
    if (!power && std::any_of(selectedIds.cbegin(), selectedIds.cend(), [this](const QString& id) {
            return m_ctx.project->mediaPool()->isOffline(id);
        }))
        menu.addAction(tr("Relink Selected Clips…"), this,
            [this, selectedIds] { relinkWithDialog(selectedIds); });
    // Replace Selected Clip: any file, also a renamed one Relink cannot find by name.
    if (!power && selectedIds.size() == 1 && currentItem
        && currentItem->kind == MediaItemKind::File)
        menu.addAction(tr("Replace Selected Clip…"), this, [this, current] {
            const QString path = platform::getOpenFileName(this, tr("Replace Selected Clip"),
                mediaFileDialogFilter(), QStringLiteral("paths/lastRelinkDir"));
            QString error;
            if (!path.isEmpty() && !replaceClip(current, path, &error))
                QMessageBox::warning(this, tr("Replace Selected Clip"), error);
        });
}

void MediaPoolPanel::fillStreamlinedItemMenu(QMenu& menu)
{
    const QStringList ids = selectedMediaIds();
    const QString current = currentMediaId();
    const MediaItem* currentItem = m_model->item(current);
    const bool power = currentItem && m_model->poolOf(current) != m_ctx.project->mediaPool();
    ActionRegistry* reg = m_ctx.actions;
    const bool onItem = !current.isEmpty();
    const QStringList selection = ids.contains(current) ? ids : QStringList {current};
    // Drops a submenu that ended up empty.
    auto dropIfEmpty = [](QMenu* sub) {
        if (sub->actions().isEmpty())
            delete sub; // removes its entry from the parent as well
    };

    if (onItem) {
        // Open, rename, find the file.
        if (currentItem && currentItem->isTimeline()) {
            const quint64 uid = currentItem->timelineUid;
            if (currentItem->kind == MediaItemKind::MulticamClip)
                menu.addAction(tr("Open in Source Viewer"), this,
                    [this, current] { emit openInSourceViewer(current); });
            menu.addAction(tr("Open in Timeline"), this, [this, uid] { openTimeline(uid); });
        } else if (!currentItem || !currentItem->isSubtitle()) {
            menu.addAction(tr("Open in Source Viewer"), this, [this, current] {
                const QStringList projectIds = ensureInProject({current});
                if (!projectIds.isEmpty())
                    emit openInSourceViewer(projectIds.front());
            });
        }
        menu.addAction(tr("Rename"), this, &MediaPoolPanel::renameCurrentItem);
        if (currentItem && !currentItem->isTimeline()) {
            const bool stored = isStoredInProject(currentItem->path);
            menu.addAction(
                stored ? tr("Reveal Project File in File Manager") : tr("Reveal in File Manager"),
                this, [this, current] { revealInFileManager(current); });
        }
        menu.addSeparator();
        // Organise.
        addColorMenu(menu, ids);
        addFlagMenu(menu, ids);
        QMenu* move = menu.addMenu(tr("Move to Bin"));
        const QString prefix
            = power ? QLatin1String(MediaPoolFilterModel::kPowerPrefix) : QString();
        const MediaPool* pool = power ? m_ctx.powerBins->pool() : m_ctx.project->mediaPool();
        move->addAction(power ? tr("Power Bins") : tr("Master"), this,
            [this, ids, prefix] { moveItemsToBin(ids, prefix); });
        for (const Bin& bin : pool->bins()) {
            const QString key = prefix + bin.id;
            move->addAction(bin.name, this, [this, ids, key] { moveItemsToBin(ids, key); });
        }
        menu.addSeparator();
        // About the media.
        if (currentItem && currentItem->kind == MediaItemKind::File) {
            menu.addAction(tr("Clip Attributes…"), this, [this, ids, current] {
                const QStringList projectIds
                    = ensureInProject(ids.contains(current) ? ids : QStringList {current});
                if (!projectIds.isEmpty())
                    clipattributes::edit(*m_ctx.project, projectIds, this);
            });
        }
        if (reg && m_ctx.media && currentItem && !currentItem->isTimeline() && !power) {
            publishSelection();
            QMenu* proxies = menu.addMenu(tr("Proxies and Optimized Media"));
            proxies->setObjectName(QStringLiteral("ProxyMenu"));
            for (const char* id : {"mediapool.generate_proxy", "mediapool.generate_optimized",
                     "mediapool.stop_generation"})
                addRegistered(*proxies, reg, id);
            proxies->addSeparator();
            for (const char* id : {"mediapool.link_proxy", "mediapool.unlink_proxy"})
                addRegistered(*proxies, reg, id);
            proxies->addSeparator();
            for (const char* id : {"mediapool.delete_proxy", "mediapool.delete_optimized"})
                addRegistered(*proxies, reg, id);
        }
        if (!power && currentItem && !currentItem->isTimeline()) {
            QMenu* multicamMenu = menu.addMenu(tr("Multicam"));
            multicamMenu->setObjectName(QStringLiteral("MulticamMenu"));
            addMulticamMenu(*multicamMenu, ids);
            dropIfEmpty(multicamMenu);
        }
        menu.addSeparator();
        menu.addAction(icon(QStringLiteral("remove")),
            selection.size() > 1 ? tr("Delete %1 Items").arg(selection.size()) : tr("Delete"), this,
            [this, selection] { confirmAndDelete(selection); });
        menu.addSeparator();
    }
    // Adding to the pool and looking after it.
    QMenu* add = menu.addMenu(tr("Add"));
    add->setObjectName(QStringLiteral("AddToPoolMenu"));
    add->addAction(icon(QStringLiteral("import")), tr("Import Media…"), this,
        &MediaPoolPanel::importWithDialog);
    if (!MediaPoolFilterModel::isSmartKey(currentBinKey())
        && !MediaPoolFilterModel::isPowerKey(currentBinKey()))
        add->addAction(icon(QStringLiteral("folder")), tr("Import Media Folder…"), this,
            [this] { importFolderWithDialog(currentBinKey()); });
    add->addAction(icon(QStringLiteral("folder")), tr("New Bin"), this,
        [this] { createBin(tr("New Bin"), currentBinKey()); });
    add->addAction(icon(QStringLiteral("smart_bin")), tr("New Smart Bin…"), this,
        &MediaPoolPanel::newSmartBinWithDialog);
    addRegistered(*add, reg, "timeline.new");
    QMenu* tools = menu.addMenu(tr("Media Tools"));
    tools->setObjectName(QStringLiteral("MediaToolsMenu"));
    if (onItem && currentItem && !currentItem->isTimeline())
        addFileRepairCommands(*tools, ids, current, power);
    if (onItem && !power && reg)
        addRegistered(*tools, reg, "mediapool.select_in_timeline");
    if (!tools->actions().isEmpty())
        tools->addSeparator();
    if (reg && reg->contains(QStringLiteral("mediapool.remove_unused")))
        addRegistered(*tools, reg, "mediapool.remove_unused");
    else
        tools->addAction(tr("Remove Unused Clips…"), this, [this] { removeUnusedClips(true); });
    // Only offered when something is offline (one stat per item when the menu opens).
    const auto& items = m_ctx.project->mediaPool()->state().items;
    if (std::any_of(items.cbegin(), items.cend(),
            [](const MediaItem& m) { return !m.isTimeline() && !media::mediaExists(m.path); }))
        tools->addAction(tr("Relink Offline Media…"), this, [this] { relinkWithDialog(); });
}

void MediaPoolPanel::showItemMenu(const QPoint& globalPos)
{
    const QStringList ids = selectedMediaIds();
    const QString current = currentMediaId();
    QMenu menu(this);
    const MediaItem* currentItem = m_model->item(current);
    const bool power = currentItem && m_model->poolOf(current) != m_ctx.project->mediaPool();
    ActionRegistry* reg = m_ctx.actions;
    if (menulayout::streamlined()) {
        fillStreamlinedItemMenu(menu);
        menu.exec(globalPos);
        return;
    }
    if (currentItem && currentItem->isTimeline()) {
        const quint64 uid = currentItem->timelineUid;
        if (currentItem->kind == MediaItemKind::MulticamClip)
            menu.addAction(tr("Open in Source Viewer"), this,
                [this, current] { emit openInSourceViewer(current); });
        menu.addAction(tr("Open in Timeline"), this, [this, uid] { openTimeline(uid); });
        menu.addAction(currentItem->kind == MediaItemKind::MulticamClip
                ? tr("Duplicate Multicam Clip")
                : currentItem->kind == MediaItemKind::CompoundClip ? tr("Duplicate Compound Clip")
                                                                   : tr("Duplicate Timeline"),
            this, [this, uid] { m_ctx.project->duplicateTimeline(uid); });
        if (currentItem->kind == MediaItemKind::Timeline) {
            // Resolve's Timelines submenu items for a timeline (spec 25.12).
            QMenu* timelines = menu.addMenu(tr("Timelines"));
            timelines->addAction(tr("Starting Timecode…"), this,
                [this, uid] { timelinedialogs::editStartingTimecode(this, *m_ctx.project, uid); });
            timelines->addAction(tr("Timeline Settings…"), this,
                [this, uid] { timelinedialogs::editTimelineSettings(this, *m_ctx.project, uid); });
        }
        menu.addSeparator();
        menu.addAction(tr("Rename"), this, &MediaPoolPanel::renameCurrentItem);
    } else if (!current.isEmpty()) {
        if (!currentItem || !currentItem->isSubtitle()) {
            menu.addAction(tr("Open in Source Viewer"), this, [this, current] {
                const QStringList projectIds = ensureInProject({current});
                if (!projectIds.isEmpty())
                    emit openInSourceViewer(projectIds.front());
            });
            menu.addSeparator();
        }
        menu.addAction(tr("Rename"), this, &MediaPoolPanel::renameCurrentItem);
        const bool stored = currentItem && isStoredInProject(currentItem->path);
        menu.addAction(
            stored ? tr("Reveal Project File in File Manager") : tr("Reveal in File Manager"), this,
            [this, current] { revealInFileManager(current); });
        addFileRepairCommands(menu, ids, current, power);
        // Clip Attributes (attrs branch) of the selected file items; power bin items are
        // copied into the project first, the dialog edits project items only.
        if (currentItem && currentItem->kind == MediaItemKind::File) {
            menu.addSeparator();
            menu.addAction(tr("Clip Attributes…"), this, [this, ids, current] {
                const QStringList projectIds
                    = ensureInProject(ids.contains(current) ? ids : QStringList {current});
                if (!projectIds.isEmpty())
                    clipattributes::edit(*m_ctx.project, projectIds, this);
            });
        }
        if (!power) {
            menu.addSeparator();
            addMulticamMenu(menu, ids);
        }
    }
    if (!current.isEmpty()) {
        addColorMenu(menu, ids);
        addFlagMenu(menu, ids);
        if (!power) {
            menu.addSeparator();
            addRegistered(menu, reg, "mediapool.select_in_timeline");
            if (!reg)
                menu.addAction(tr("Select Clips in Timeline"), this,
                    [this, ids] { selectClipsInTimeline(ids); });
        }
        QMenu* move = menu.addMenu(tr("Move to Bin"));
        const QString prefix
            = power ? QLatin1String(MediaPoolFilterModel::kPowerPrefix) : QString();
        const MediaPool* pool = power ? m_ctx.powerBins->pool() : m_ctx.project->mediaPool();
        move->addAction(power ? tr("Power Bins") : tr("Master"), this,
            [this, ids, prefix] { moveItemsToBin(ids, prefix); });
        for (const Bin& bin : pool->bins()) {
            const QString key = prefix + bin.id;
            move->addAction(bin.name, this, [this, ids, key] { moveItemsToBin(ids, key); });
        }
        // Proxy / optimized media (Resolve's media pool context menu entries).
        if (reg && m_ctx.media && currentItem && !currentItem->isTimeline() && !power) {
            publishSelection();
            menu.addSeparator();
            for (const char* id : {"mediapool.generate_optimized", "mediapool.generate_proxy",
                     "mediapool.stop_generation", "mediapool.link_proxy", "mediapool.unlink_proxy",
                     "mediapool.delete_proxy", "mediapool.delete_optimized"})
                addRegistered(menu, reg, id);
        }
        menu.addSeparator();
        menu.addAction(icon(QStringLiteral("remove")),
            ids.size() > 1 ? tr("Delete %1 Items").arg(ids.size()) : tr("Delete"), this,
            [this, ids] { confirmAndDelete(ids); });
        menu.addSeparator();
    }
    menu.addAction(icon(QStringLiteral("import")), tr("Import Media…"), this,
        &MediaPoolPanel::importWithDialog);
    if (!MediaPoolFilterModel::isSmartKey(currentBinKey())
        && !MediaPoolFilterModel::isPowerKey(currentBinKey()))
        menu.addAction(icon(QStringLiteral("folder")), tr("Import Media Folder…"), this,
            [this] { importFolderWithDialog(currentBinKey()); });
    menu.addAction(icon(QStringLiteral("folder")), tr("New Bin"), this,
        [this] { createBin(tr("New Bin"), currentBinKey()); });
    menu.addAction(icon(QStringLiteral("smart_bin")), tr("New Smart Bin…"), this,
        &MediaPoolPanel::newSmartBinWithDialog);
    addRegistered(menu, reg, "timeline.new");
    menu.addSeparator();
    if (reg && reg->contains(QStringLiteral("mediapool.remove_unused")))
        addRegistered(menu, reg, "mediapool.remove_unused");
    else
        menu.addAction(tr("Remove Unused Clips…"), this, [this] { removeUnusedClips(true); });
    // Only offered when something is offline (one stat per item when the menu opens).
    const auto& items = m_ctx.project->mediaPool()->state().items;
    if (std::any_of(items.cbegin(), items.cend(),
            [](const MediaItem& m) { return !m.isTimeline() && !media::mediaExists(m.path); })) {
        menu.addSeparator();
        menu.addAction(tr("Relink Offline Media…"), this, [this] { relinkWithDialog(); });
    }
    menu.exec(globalPos);
}

void MediaPoolPanel::showFolderMenu(
    QAbstractItemView* view, const QModelIndex& index, const QPoint& globalPos)
{
    // A folder in the clip area: the bin commands of the bin list, acting on that bin.
    const QString bin = index.data(MediaPoolModel::FolderBinRole).toString();
    if (!m_selection->isSelected(index))
        m_selection->setCurrentIndex(
            index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("FolderMenu"));
    menu.addAction(
        icon(QStringLiteral("folder")), tr("Open"), this, [this, bin] { setCurrentBinKey(bin); });
    menu.addSeparator();
    const QPersistentModelIndex name = index.siblingAtColumn(MediaPoolModel::NameColumn);
    menu.addAction(tr("Rename Bin"), this, [view, name] {
        if (name.isValid())
            view->edit(name);
    });
    // Several selected folders delete together; otherwise the one under the pointer.
    QStringList bins = selectedBinIds();
    if (!bins.contains(bin))
        bins = {bin};
    menu.addAction(icon(QStringLiteral("remove")),
        bins.size() > 1 ? tr("Delete %1 Bins").arg(bins.size()) : tr("Delete Bin"), this,
        [this, bins] {
            for (const QString& b : bins)
                deleteBin(b, true);
        });
    menu.addSeparator();
    menu.addAction(icon(QStringLiteral("folder")), tr("New Bin"), this,
        [this] { createBin(tr("New Bin"), currentBinKey()); });
    menu.addAction(icon(QStringLiteral("import")), tr("Import Media Folder…"), this,
        [this, bin] { importFolderWithDialog(bin); });
    menu.addAction(icon(QStringLiteral("smart_bin")), tr("New Smart Bin…"), this,
        &MediaPoolPanel::newSmartBinWithDialog);
    menu.exec(globalPos);
}

void MediaPoolPanel::showBinMenu(QTreeWidgetItem* item, const QPoint& globalPos)
{
    const QString key = item ? item->data(0, BinList::kKeyRole).toString() : QString();
    const BinList::RowKind kind = item ? BinList::kindOf(item) : BinList::RowKind::Master;
    QMenu menu(this);
    switch (kind) {
    case BinList::RowKind::SmartHeader:
        menu.addAction(icon(QStringLiteral("smart_bin")), tr("New Smart Bin…"), this,
            &MediaPoolPanel::newSmartBinWithDialog);
        break;
    case BinList::RowKind::SmartBin: {
        const SmartBin* smart = m_proxy->smartBinFor(key);
        const bool user = smart && m_ctx.project->mediaPool()->smartBin(smart->id);
        menu.addAction(icon(QStringLiteral("smart_bin")), tr("New Smart Bin…"), this,
            &MediaPoolPanel::newSmartBinWithDialog);
        if (user) {
            menu.addAction(
                tr("Edit Smart Bin…"), this, [this, key] { editSmartBinWithDialog(key); });
            menu.addAction(tr("Rename Smart Bin"), this, [this, item] { m_bins->editItem(item); });
            menu.addAction(icon(QStringLiteral("remove")), tr("Delete Smart Bin"), this,
                [this, key] { deleteSmartBin(key); });
        } else {
            // A default smart bin: copy its rules into a new, editable one.
            if (smart) {
                const SmartBin copy {QString(), smart->name, smart->matchAll, smart->rules};
                menu.addAction(tr("Duplicate as New Smart Bin"), this,
                    [this, copy] { setCurrentBinKey(createSmartBin(copy)); });
            }
        }
        break;
    }
    default:
        menu.addAction(icon(QStringLiteral("folder")), tr("New Bin"), this,
            [this, key] { createBin(tr("New Bin"), key); });
        // Project bins (and Master) only: power bins hold no folder structure of their own.
        if (kind != BinList::RowKind::PowerRoot && kind != BinList::RowKind::PowerBin)
            menu.addAction(icon(QStringLiteral("import")), tr("Import Media Folder…"), this,
                [this, key] { importFolderWithDialog(key); });
        if (kind != BinList::RowKind::PowerRoot && kind != BinList::RowKind::PowerBin)
            menu.addAction(icon(QStringLiteral("smart_bin")), tr("New Smart Bin…"), this,
                &MediaPoolPanel::newSmartBinWithDialog);
        // Streamlined (spec 82): making bins, then changing this one, then the bin list.
        if (menulayout::streamlined())
            menu.addSeparator();
        if (kind == BinList::RowKind::ProjectBin || kind == BinList::RowKind::PowerBin) {
            menu.addAction(tr("Rename Bin"), this, [this, item] { m_bins->editItem(item); });
            menu.addAction(icon(QStringLiteral("remove")), tr("Delete Bin"), this,
                [this, key] { deleteBin(key, true); });
        }
        break;
    }
    menu.addSeparator();
    QAction* toggle
        = menu.addAction(tr("Show Bin List"), this, [this](bool on) { setBinsVisible(on); });
    toggle->setCheckable(true);
    toggle->setChecked(binsVisible());
    menu.exec(globalPos);
}

int MediaPoolPanel::relinkFromFolder(const QString& folder, const QStringList& onlyIds)
{
    Project* project = m_ctx.project;
    QStringList wereOffline;
    for (const MediaItem& m : project->mediaPool()->state().items) {
        if (!m.isTimeline() && (onlyIds.isEmpty() || onlyIds.contains(m.id))
            && project->mediaPool()->isOffline(m.id))
            wereOffline.append(m.id);
    }
    project->beginEdit(tr("Relink Media"));
    const int relinked = project->mediaPool()->relinkFromFolder(folder, onlyIds);
    project->endEdit();
    // A single-file project keeps its media inside: the found files are copied in.
    if (relinked > 0 && !wereOffline.isEmpty())
        storeItemsInProject(m_ctx, wereOffline, this);
    m_model->recheckOffline();
    return relinked;
}

bool MediaPoolPanel::replaceClip(const QString& id, const QString& path, QString* error)
{
    Project* project = m_ctx.project;
    const MediaItem* item = project->mediaPool()->item(id);
    if (!item || item->kind != MediaItemKind::File)
        return false;
    const QString absolute = QFileInfo(path).absoluteFilePath();
    PreparedMedia prepared = MediaPool::prepareFile(absolute);
    if (!prepared.ok || prepared.item.kind != MediaItemKind::File) {
        if (error)
            *error = tr("%1 cannot be used: %2")
                         .arg(QFileInfo(absolute).fileName(),
                             prepared.ok ? tr("not a video, audio or image file") : prepared.error);
        return false;
    }
    // A name that was the file name follows the new file; a name given by the user stays.
    // Compared without case: "CLIP.MOV" shown as "clip.mov" is still the file name.
    const bool fileName
        = item->name.compare(QFileInfo(item->path).fileName(), Qt::CaseInsensitive) == 0;
    const QString name = fileName ? QFileInfo(absolute).fileName() : item->name;
    project->beginEdit(tr("Replace Clip"));
    project->mediaPool()->relinkItem(id, absolute);
    project->mediaPool()->updateItem(id, name, prepared.item.info);
    project->endEdit();
    // A single-file project keeps its media inside: the new file is copied in.
    if (m_ctx.mediaStore && m_ctx.mediaStore->storesMedia())
        storeItemsInProject(m_ctx, {id}, this);
    m_model->recheckOffline();
    return true;
}

void MediaPoolPanel::relinkWithDialog(const QStringList& onlyIds)
{
    const QString folder = platform::getExistingDirectory(this,
        tr("Relink Offline Media: Choose the Folder with the Files"),
        QStringLiteral("paths/lastRelinkDir"));
    if (folder.isEmpty())
        return;
    const int relinked = relinkFromFolder(folder, onlyIds);
    const auto& items = m_ctx.project->mediaPool()->state().items;
    const auto stillOffline = std::count_if(items.cbegin(), items.cend(), [&](const MediaItem& m) {
        return !m.isTimeline() && (onlyIds.isEmpty() || onlyIds.contains(m.id))
            && !media::mediaExists(m.path);
    });
    QMessageBox::information(this, tr("Relink Offline Media"),
        plural(relinked, tr("%n item relinked."), tr("%n items relinked."))
            + (stillOffline > 0 ? QLatin1Char(' ')
                        + plural(stillOffline,
                            tr("%n item is still offline (no file with the same name in "
                               "that folder)."),
                            tr("%n items are still offline (no file with the same name in "
                               "that folder)."))
                                : QString()));
}

} // namespace kk
