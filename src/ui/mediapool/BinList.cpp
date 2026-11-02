// SPDX-License-Identifier: GPL-3.0-or-later
// BinList implementation: rows built from the pools, drag and drop of bins, items and files.
#include "ui/mediapool/BinList.h"

#include "core/MediaPool.h"
#include "core/SmartBins.h"
#include "platform/DroppedFiles.h"
#include "ui/common/DragAndDrop.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/mediapool/MediaPoolModel.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QSet>
#include <QTimer>

namespace kk {

namespace {

/// Fraction of a row's height at its top / bottom edge that means "before" / "after".
constexpr double kEdgeFraction = 0.25;

QStringList lines(const QMimeData* data, const char* format)
{
    return QString::fromUtf8(data->data(QLatin1String(format)))
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

bool isPowerKind(BinList::RowKind k)
{
    return k == BinList::RowKind::PowerRoot || k == BinList::RowKind::PowerBin;
}

bool isProjectKind(BinList::RowKind k)
{
    return k == BinList::RowKind::Master || k == BinList::RowKind::ProjectBin;
}

} // namespace

BinList::BinList(QWidget* parent)
    : QTreeWidget(parent)
{
    setHeaderHidden(true);
    setFrameShape(QFrame::NoFrame);
    setIndentation(12);
    setAcceptDrops(true);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(false); // drawn by paintEvent() (own drop logic)
    setEditTriggers(QAbstractItemView::EditKeyPressed);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (m_rebuilding || !item)
            return;
        if (kindOf(item) == RowKind::SmartHeader)
            return;
        emit keyActivated(item->data(0, kKeyRole).toString());
    });
}

BinList::RowKind BinList::kindOf(const QTreeWidgetItem* item)
{
    return item ? RowKind(item->data(0, kKindRole).toInt()) : RowKind::Master;
}

QTreeWidgetItem* BinList::addRow(QTreeWidgetItem* parent, const QString& text, const QString& key,
    RowKind kind, const QString& iconName)
{
    auto* item = parent ? new QTreeWidgetItem(parent, {text}) : new QTreeWidgetItem(this, {text});
    item->setIcon(0, icon(iconName));
    item->setData(0, kKeyRole, key);
    item->setData(0, kKindRole, int(kind));
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
    switch (kind) {
    case RowKind::ProjectBin:
    case RowKind::PowerBin:
        flags |= Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
        break;
    case RowKind::SmartBin:
        flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!MediaPoolFilterModel::idOfKey(key).startsWith(
                QLatin1String(smartbins::kDefaultIdPrefix)))
            flags |= Qt::ItemIsEditable; // user smart bins rename in place
        break;
    case RowKind::SmartHeader:
        flags = Qt::ItemIsEnabled;
        break;
    default:
        break;
    }
    item->setFlags(flags);
    return item;
}

void BinList::rebuild(
    const MediaPool& project, const QVector<SmartBin>& smart, const MediaPool* power)
{
    // Bins are few (tens): a full rebuild is simpler than incremental updates; it keeps the
    // current row and the collapsed rows.
    m_rebuilding = true;
    QSet<QString> collapsed;
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        if (!(*it)->isExpanded())
            collapsed.insert((*it)->data(0, kKeyRole).toString() + QLatin1Char('|')
                + QString::number((*it)->data(0, kKindRole).toInt()));
    }
    const QString current = currentKey();
    clear();

    // A bin tree of one pool: children in their stored order, recursively. Bins with a
    // dangling parent id are shown at the top level; cycles are cut by the visited set.
    auto addTree = [this](const MediaPool& pool, QTreeWidgetItem* root, const QString& prefix,
                       RowKind kind) {
        QHash<QString, QVector<const Bin*>> children;
        for (const Bin& b : pool.bins()) {
            const QString parent
                = b.parentId.isEmpty() || pool.bin(b.parentId) ? b.parentId : QString();
            children[parent].append(&b);
        }
        QSet<QString> visited;
        std::function<void(const QString&, QTreeWidgetItem*)> add
            = [&](const QString& parentId, QTreeWidgetItem* parentItem) {
                  for (const Bin* b : children.value(parentId)) {
                      if (visited.contains(b->id))
                          continue;
                      visited.insert(b->id);
                      QTreeWidgetItem* row = addRow(
                          parentItem, b->name, prefix + b->id, kind, QStringLiteral("folder"));
                      add(b->id, row);
                  }
              };
        add(QString(), root);
    };

    QTreeWidgetItem* master
        = addRow(nullptr, tr("Master"), QString(), RowKind::Master, QStringLiteral("folder"));
    addTree(project, master, QString(), RowKind::ProjectBin);

    QTreeWidgetItem* smartHeader = addRow(nullptr, tr("Smart Bins"), QStringLiteral("smart-header"),
        RowKind::SmartHeader, QStringLiteral("smart_bin"));
    for (const SmartBin& b : smart) {
        addRow(smartHeader, b.name, QLatin1String(MediaPoolFilterModel::kSmartPrefix) + b.id,
            RowKind::SmartBin, QStringLiteral("smart_bin"));
    }
    if (smart.isEmpty())
        smartHeader->setForeground(0, palette().brush(QPalette::Disabled, QPalette::Text));

    if (power) {
        QTreeWidgetItem* powerRoot
            = addRow(nullptr, tr("Power Bins"), QLatin1String(MediaPoolFilterModel::kPowerPrefix),
                RowKind::PowerRoot, QStringLiteral("power_bin"));
        addTree(*power, powerRoot, QLatin1String(MediaPoolFilterModel::kPowerPrefix),
            RowKind::PowerBin);
    }

    QTreeWidgetItemIterator all(this);
    for (; *all; ++all) {
        const QString id = (*all)->data(0, kKeyRole).toString() + QLatin1Char('|')
            + QString::number((*all)->data(0, kKindRole).toInt());
        (*all)->setExpanded(!collapsed.contains(id));
    }
    QTreeWidgetItem* cur = itemForKey(current);
    setCurrentItem(cur ? cur : master);
    m_rebuilding = false;
}

QString BinList::currentKey() const
{
    const QTreeWidgetItem* item = currentItem();
    return item && kindOf(item) != RowKind::SmartHeader ? item->data(0, kKeyRole).toString()
                                                        : QString();
}

QTreeWidgetItem* BinList::itemForKey(const QString& key) const
{
    QTreeWidgetItemIterator it(const_cast<BinList*>(this));
    for (; *it; ++it) {
        if (kindOf(*it) != RowKind::SmartHeader && (*it)->data(0, kKeyRole).toString() == key)
            return *it;
    }
    return nullptr;
}

bool BinList::setCurrentKey(const QString& key)
{
    QTreeWidgetItem* item = itemForKey(key);
    if (!item)
        return false;
    const bool was = m_rebuilding;
    m_rebuilding = true;
    setCurrentItem(item);
    m_rebuilding = was;
    return true;
}

// ---- Drag and drop ----

QStringList BinList::mimeTypes() const
{
    return {QLatin1String(kBinMime)};
}

QMimeData* BinList::mimeData(const QList<QTreeWidgetItem*>& items) const
{
    if (items.isEmpty())
        return nullptr;
    const RowKind kind = kindOf(items.front());
    if (kind != RowKind::ProjectBin && kind != RowKind::PowerBin)
        return nullptr;
    auto* data = new QMimeData;
    data->setData(QLatin1String(kBinMime), items.front()->data(0, kKeyRole).toString().toUtf8());
    return data;
}

QTreeWidgetItem* BinList::dropTarget(
    const QMimeData* data, const QPoint& pos, DropPlace* place) const
{
    QTreeWidgetItem* target = itemAt(pos);
    if (!target)
        return nullptr;
    const RowKind kind = kindOf(target);
    *place = DropPlace::Into;
    if (data->hasFormat(QLatin1String(kBinMime))) {
        const QString key = QString::fromUtf8(data->data(QLatin1String(kBinMime)));
        const QTreeWidgetItem* dragged = itemForKey(key);
        if (!dragged)
            return nullptr;
        // Bins stay in their section (project bins vs power bins).
        const bool power = isPowerKind(kindOf(dragged));
        if (power ? !isPowerKind(kind) : !isProjectKind(kind))
            return nullptr;
        // Edges of a real bin row mean before / after it; roots only take "into".
        if (kind == RowKind::ProjectBin || kind == RowKind::PowerBin) {
            const QRect r = visualItemRect(target);
            const int edge = int(r.height() * kEdgeFraction);
            if (pos.y() < r.top() + edge)
                *place = DropPlace::Before;
            else if (pos.y() >= r.bottom() - edge)
                *place = DropPlace::After;
        }
        // Not into itself or its own sub-bins.
        for (const QTreeWidgetItem* p = target; p; p = p->parent()) {
            if (p == dragged && (*place == DropPlace::Into || p != target))
                return nullptr;
        }
        return target;
    }
    // Items and files go into bins of either section, not into smart bins.
    if (kind == RowKind::SmartBin || kind == RowKind::SmartHeader)
        return nullptr;
    return target;
}

void BinList::dragEnterEvent(QDragEnterEvent* event)
{
    const QMimeData* data = event->mimeData();
    if (data->hasFormat(QLatin1String(mime::MediaItems)) || data->hasFormat(QLatin1String(kBinMime))
        || platform::hasDroppedFiles(data))
        event->acceptProposedAction();
    else
        event->ignore();
    logDropEvent("bins", event);
}

void BinList::dragMoveEvent(QDragMoveEvent* event)
{
    DropPlace place = DropPlace::Into;
    QTreeWidgetItem* target = dropTarget(event->mimeData(), event->position().toPoint(), &place);
    const QRect oldLine = m_dropLine;
    m_dropLine = {};
    if (target) {
        if (place == DropPlace::Into) {
            const bool was = m_rebuilding;
            m_rebuilding = true; // highlight the target without switching the shown bin
            setCurrentItem(target);
            m_rebuilding = was;
        } else {
            const QRect r = visualItemRect(target);
            const int y = place == DropPlace::Before ? r.top() : r.bottom();
            m_dropLine = QRect(r.left(), y - 1, viewport()->width() - r.left(), 2);
        }
        event->setDropAction(event->mimeData()->hasFormat(QLatin1String(kBinMime))
                ? Qt::MoveAction
                : event->proposedAction());
        event->accept();
    } else {
        event->ignore();
    }
    if (oldLine != m_dropLine)
        viewport()->update();
    logDropEvent("bins", event);
}

void BinList::dragLeaveEvent(QDragLeaveEvent* event)
{
    m_dropLine = {};
    viewport()->update();
    QTreeWidget::dragLeaveEvent(event);
}

void BinList::dropEvent(QDropEvent* event)
{
    logDropEvent("bins", event);
    m_dropLine = {};
    viewport()->update();
    const QMimeData* data = event->mimeData();
    DropPlace place = DropPlace::Into;
    QTreeWidgetItem* target = dropTarget(data, event->position().toPoint(), &place);
    if (!target) {
        event->ignore();
        return;
    }
    const QString key = target->data(0, kKeyRole).toString();
    if (data->hasFormat(QLatin1String(kBinMime))) {
        const QString dragged = QString::fromUtf8(data->data(QLatin1String(kBinMime)));
        event->setDropAction(Qt::MoveAction);
        event->accept();
        // From the event loop: the callback rebuilds this widget, which must not happen while
        // QAbstractItemView is still inside its drag.
        QTimer::singleShot(0, this, [this, dragged, key, place] {
            if (onBinDropped)
                onBinDropped(dragged, key, place);
        });
        return;
    }
    if (data->hasFormat(QLatin1String(mime::MediaItems))) {
        // Power bin items carry their own ids; reading mime::MediaItems for them would copy
        // them into the project, so the power list is read first.
        const QStringList power = lines(data, kPowerItemsMime);
        QStringList ids;
        if (power.isEmpty())
            ids = lines(data, mime::MediaItems);
        event->acceptProposedAction();
        if (onItemsDropped)
            onItemsDropped(ids, power, key);
        return;
    }
    if (platform::hasDroppedFiles(data) && onFilesDropped) {
        // Deferred like MediaPoolPanel::dropEvent(): no modal dialog inside the drop.
        const QStringList paths = platform::droppedLocalFiles(data);
        QTimer::singleShot(0, this, [this, paths, key] { onFilesDropped(paths, key); });
    }
    event->acceptProposedAction();
}

void BinList::paintEvent(QPaintEvent* event)
{
    QTreeWidget::paintEvent(event);
    if (m_dropLine.isEmpty())
        return;
    QPainter p(viewport());
    p.fillRect(m_dropLine, palette().color(QPalette::Highlight));
}

} // namespace kk
