// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPool implementation: import via media::probe(), bin management, change signals.
#include "core/MediaPool.h"

#include "core/Logging.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>

namespace kk {

namespace {
QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
} // namespace

MediaPool::MediaPool(QObject* parent)
    : QObject(parent)
{ }

int MediaPool::indexOfItem(const QString& id) const
{
    for (int i = 0; i < m_state.items.size(); ++i) {
        if (m_state.items[i].id == id)
            return i;
    }
    return -1;
}

int MediaPool::indexOfBin(const QString& id) const
{
    for (int i = 0; i < m_state.bins.size(); ++i) {
        if (m_state.bins[i].id == id)
            return i;
    }
    return -1;
}

void MediaPool::touch()
{
    ++m_revision;
    emit changed();
}

const MediaItem* MediaPool::item(const QString& id) const
{
    const int i = indexOfItem(id);
    return i < 0 ? nullptr : &m_state.items[i];
}

const Bin* MediaPool::bin(const QString& id) const
{
    const int i = indexOfBin(id);
    return i < 0 ? nullptr : &m_state.bins[i];
}

QStringList MediaPool::itemIdsInBin(const QString& binId) const
{
    QStringList ids;
    for (const MediaItem& m : m_state.items) {
        if (m.binId == binId)
            ids.append(m.id);
    }
    return ids;
}

ImportResult MediaPool::importFiles(const QStringList& paths, const QString& binId)
{
    ImportResult result;
    const QString targetBin = binId.isEmpty() || bin(binId) ? binId : QString();
    for (const QString& path : paths) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        const bool duplicate = std::any_of(m_state.items.cbegin(), m_state.items.cend(),
            [&](const MediaItem& m) { return m.path == absolute; });
        if (duplicate) {
            result.warnings.append(tr("%1: already in the media pool").arg(absolute));
            continue;
        }
        media::MediaInfo info = media::probe(absolute);
        if (!info.valid) {
            const QString reason = info.error.isEmpty() ? tr("unsupported file") : info.error;
            result.warnings.append(tr("%1: %2").arg(absolute, reason));
            qCWarning(lcCore) << "import skipped" << absolute << reason;
            continue;
        }
        MediaItem item;
        item.path = absolute;
        item.binId = targetBin;
        item.info = std::move(info);
        result.importedIds.append(addItem(std::move(item)));
    }
    return result;
}

QString MediaPool::addItem(MediaItem item)
{
    if (item.id.isEmpty() || indexOfItem(item.id) >= 0)
        item.id = newId();
    if (item.name.isEmpty())
        item.name = QFileInfo(item.path).fileName();
    if (!item.binId.isEmpty() && !bin(item.binId))
        item.binId.clear();
    const QString id = item.id;
    m_state.items.append(std::move(item));
    emit itemAdded(id);
    touch();
    return id;
}

bool MediaPool::removeItem(const QString& id)
{
    const int i = indexOfItem(id);
    if (i < 0)
        return false;
    m_state.items.remove(i);
    emit itemRemoved(id);
    touch();
    return true;
}

int MediaPool::relinkFromFolder(const QString& folder)
{
    const QDir dir(folder);
    int relinked = 0;
    for (MediaItem& item : m_state.items) {
        if (QFileInfo::exists(item.path))
            continue;
        const QString candidate = dir.filePath(QFileInfo(item.path).fileName());
        if (!QFileInfo(candidate).isFile())
            continue;
        item.path = QFileInfo(candidate).absoluteFilePath();
        item.info.path = item.path;
        emit itemChanged(item.id);
        ++relinked;
    }
    if (relinked > 0)
        touch();
    return relinked;
}

bool MediaPool::renameItem(const QString& id, const QString& name)
{
    const int i = indexOfItem(id);
    if (i < 0)
        return false;
    if (m_state.items[i].name == name)
        return true;
    m_state.items[i].name = name;
    emit itemChanged(id);
    touch();
    return true;
}

bool MediaPool::moveItemToBin(const QString& itemId, const QString& binId)
{
    const int i = indexOfItem(itemId);
    if (i < 0 || (!binId.isEmpty() && !bin(binId)))
        return false;
    if (m_state.items[i].binId == binId)
        return true;
    m_state.items[i].binId = binId;
    emit itemChanged(itemId);
    touch();
    return true;
}

QString MediaPool::addBin(const QString& name, const QString& parentId)
{
    Bin b;
    b.id = newId();
    b.name = name;
    b.parentId = parentId.isEmpty() || bin(parentId) ? parentId : QString();
    m_state.bins.append(b);
    emit binsChanged();
    touch();
    return b.id;
}

bool MediaPool::removeBin(const QString& id)
{
    const int i = indexOfBin(id);
    if (i < 0)
        return false;
    const QString parent = m_state.bins[i].parentId;
    m_state.bins.remove(i);
    for (Bin& b : m_state.bins) {
        if (b.parentId == id)
            b.parentId = parent;
    }
    for (MediaItem& m : m_state.items) {
        if (m.binId == id) {
            m.binId = parent;
            emit itemChanged(m.id);
        }
    }
    emit binsChanged();
    touch();
    return true;
}

bool MediaPool::renameBin(const QString& id, const QString& name)
{
    const int i = indexOfBin(id);
    if (i < 0)
        return false;
    m_state.bins[i].name = name;
    emit binsChanged();
    touch();
    return true;
}

void MediaPool::setState(MediaPoolState state)
{
    m_state = std::move(state);
    emit reset();
    touch();
}

} // namespace kk
