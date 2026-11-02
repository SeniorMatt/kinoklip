// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel context menus: items (clip colour, flags, usage commands, proxies), bins,
// list header (columns), clip colour filter; plus the dialogs they open.
#include "core/ClipColors.h"
#include "core/MediaPool.h"
#include "core/PowerBins.h"
#include "core/Project.h"
#include "platform/ChildProcessEnv.h"
#include "platform/FileDialogs.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/MediaImport.h"
#include "ui/mediapool/BinList.h"
#include "ui/mediapool/MediaPoolPanel.h"

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
    for (const PaletteColor& c : clipColorPalette()) {
        const QString colorId = QLatin1String(c.id);
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
        importFiles(
            paths, MediaPoolFilterModel::isSmartKey(currentBinKey()) ? QString() : currentBinKey());
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
            tr("%n clip(s) on the timeline use this media and will show as offline. Delete anyway?",
                nullptr, used));
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
    // Opening the containing folder works on every desktop; selecting the file itself would
    // need the platform-specific FileManager1 D-Bus interface (TODO(panels): src/platform).
    // platform::openUrl: the file manager must start with the user's environment, not the
    // bundle's library/plugin paths.
    platform::openUrl(QUrl::fromLocalFile(QFileInfo(item->path).absolutePath()));
}

void MediaPoolPanel::showItemMenu(const QPoint& globalPos)
{
    const QStringList ids = selectedMediaIds();
    const QString current = currentMediaId();
    QMenu menu(this);
    const MediaItem* currentItem = m_model->item(current);
    const bool power = currentItem && m_model->poolOf(current) != m_ctx.project->mediaPool();
    ActionRegistry* reg = m_ctx.actions;
    if (currentItem && currentItem->isTimeline()) {
        const quint64 uid = currentItem->timelineUid;
        menu.addAction(tr("Open in Timeline"), this, [this, uid] { openTimeline(uid); });
        menu.addAction(currentItem->kind == MediaItemKind::CompoundClip
                ? tr("Duplicate Compound Clip")
                : tr("Duplicate Timeline"),
            this, [this, uid] { m_ctx.project->duplicateTimeline(uid); });
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
        menu.addAction(
            tr("Reveal in File Manager"), this, [this, current] { revealInFileManager(current); });
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
            [](const MediaItem& m) { return !m.isTimeline() && !QFileInfo::exists(m.path); })) {
        menu.addSeparator();
        menu.addAction(tr("Relink Offline Media…"), this, &MediaPoolPanel::relinkWithDialog);
    }
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
        if (kind != BinList::RowKind::PowerRoot && kind != BinList::RowKind::PowerBin)
            menu.addAction(icon(QStringLiteral("smart_bin")), tr("New Smart Bin…"), this,
                &MediaPoolPanel::newSmartBinWithDialog);
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

int MediaPoolPanel::relinkFromFolder(const QString& folder)
{
    Project* project = m_ctx.project;
    project->beginEdit(tr("Relink Media"));
    const int relinked = project->mediaPool()->relinkFromFolder(folder);
    project->endEdit();
    m_model->recheckOffline();
    return relinked;
}

void MediaPoolPanel::relinkWithDialog()
{
    const QString folder = platform::getExistingDirectory(this,
        tr("Relink Offline Media: Choose the Folder with the Files"),
        QStringLiteral("paths/lastRelinkDir"));
    if (folder.isEmpty())
        return;
    const int relinked = relinkFromFolder(folder);
    const auto& items = m_ctx.project->mediaPool()->state().items;
    const auto stillOffline = std::count_if(items.cbegin(), items.cend(),
        [](const MediaItem& m) { return !m.isTimeline() && !QFileInfo::exists(m.path); });
    QMessageBox::information(this, tr("Relink Offline Media"),
        tr("%n item(s) relinked.", nullptr, relinked)
            + (stillOffline > 0 ? QLatin1Char(' ')
                        + tr("%n item(s) are still offline (no file with the same name in "
                             "that folder).",
                            nullptr, int(stillOffline))
                                : QString()));
}

} // namespace kk
