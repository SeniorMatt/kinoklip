// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPool implementation: import via media::probe(), bin management, change signals.
#include "core/MediaPool.h"

#include "core/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace kk {

namespace {
QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/// Modification time of @p path in ms since the epoch (0 if the file does not exist).
qint64 fileModifiedMs(const QString& path)
{
    const QFileInfo fi(path);
    return fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
}
} // namespace

media::MediaInfo subtitleInfo(const QString& path, const QVector<SubtitleCue>& cues)
{
    media::MediaInfo info;
    info.valid = true;
    info.path = path;
    info.durationUs = subtitles::cuesDurationUs(cues);
    info.container = QFileInfo(path).suffix().toLower();
    return info;
}

namespace { } // namespace

MediaPool::MediaPool(QObject* parent)
    : QObject(parent)
{ }

int MediaPool::indexOfItem(const QString& id) const
{
    if (!m_indexValid) {
        m_index.clear();
        m_index.reserve(m_state.items.size());
        for (int i = 0; i < m_state.items.size(); ++i)
            m_index.insert(m_state.items[i].id, i);
        m_indexValid = true;
    }
    return m_index.value(id, -1);
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

const MediaItem* MediaPool::itemByPath(const QString& path) const
{
    for (const MediaItem& m : m_state.items) {
        if (!m.isTimeline() && m.path == path)
            return &m;
    }
    return nullptr;
}

QStringList MediaPool::childBins(const QString& parentId) const
{
    QStringList ids;
    for (const Bin& b : m_state.bins) {
        if (b.parentId == parentId)
            ids.append(b.id);
    }
    return ids;
}

bool MediaPool::binIsInside(const QString& binId, const QString& ancestorId) const
{
    if (ancestorId.isEmpty())
        return true;
    // Walk up the parents; the step limit guards against cycles in damaged files.
    QString current = binId;
    for (int steps = 0; steps <= m_state.bins.size(); ++steps) {
        if (current == ancestorId)
            return true;
        const Bin* b = bin(current);
        if (!b)
            return false;
        current = b->parentId;
    }
    return false;
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
        if (subtitles::isSubtitleFile(absolute)) {
            // Subtitle files are parsed, not probed: the captions are stored in the item.
            MediaItem item;
            QString error;
            if (!subtitles::readFile(absolute, &item.cues, &error)) {
                result.warnings.append(tr("%1: %2").arg(absolute, error));
                continue;
            }
            item.kind = MediaItemKind::Subtitle;
            item.path = absolute;
            item.binId = targetBin;
            item.info = subtitleInfo(absolute, item.cues);
            result.importedIds.append(addItem(std::move(item)));
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
        item.fileModifiedMs = fileModifiedMs(absolute);
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
    if (item.addedMs == 0)
        item.addedMs = QDateTime::currentMSecsSinceEpoch();
    const QString id = item.id;
    m_state.items.append(std::move(item));
    m_index.insert(id, int(m_state.items.size()) - 1); // appending keeps every other index
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
    invalidateIndex();
    emit itemRemoved(id);
    touch();
    return true;
}

bool MediaPool::updateItem(const QString& id, const QString& name, const media::MediaInfo& info)
{
    const int i = indexOfItem(id);
    if (i < 0)
        return false;
    MediaItem& item = m_state.items[i];
    if (item.name == name && sameMediaInfo(item.info, info))
        return true;
    item.name = name;
    item.info = info;
    emit itemChanged(id);
    touch();
    return true;
}

int MediaPool::relinkFromFolder(const QString& folder)
{
    const QDir dir(folder);
    int relinked = 0;
    for (MediaItem& item : m_state.items) {
        if (item.isTimeline() || QFileInfo::exists(item.path))
            continue;
        const QString candidate = dir.filePath(QFileInfo(item.path).fileName());
        if (!QFileInfo(candidate).isFile())
            continue;
        item.path = QFileInfo(candidate).absoluteFilePath();
        item.info.path = item.path;
        item.fileModifiedMs = fileModifiedMs(item.path);
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

bool MediaPool::moveBin(const QString& id, const QString& parentId, int index)
{
    const int from = indexOfBin(id);
    if (from < 0 || (!parentId.isEmpty() && !bin(parentId)) || binIsInside(parentId, id))
        return false;
    Bin moved = m_state.bins[from];
    const QStringList siblingsBefore = childBins(parentId);
    m_state.bins.remove(from);
    moved.parentId = parentId;
    // Siblings without the moved bin, in order: insert before the one now at @p index.
    QStringList siblings = siblingsBefore;
    siblings.removeAll(id);
    int insertAt = int(m_state.bins.size());
    if (index >= 0 && index < siblings.size())
        insertAt = indexOfBin(siblings[index]);
    else if (!siblings.isEmpty())
        insertAt = indexOfBin(siblings.back()) + 1;
    m_state.bins.insert(insertAt, moved);
    emit binsChanged();
    touch();
    return true;
}

QStringList MediaPool::removeBinWithContents(const QString& id)
{
    if (indexOfBin(id) < 0)
        return {};
    QSet<QString> doomed;
    for (const Bin& b : m_state.bins) {
        if (binIsInside(b.id, id))
            doomed.insert(b.id);
    }
    QStringList removed;
    for (qsizetype i = m_state.items.size() - 1; i >= 0; --i) {
        MediaItem& m = m_state.items[i];
        if (!doomed.contains(m.binId))
            continue;
        if (m.isTimeline()) {
            m.binId.clear();
            emit itemChanged(m.id);
            continue;
        }
        removed.prepend(m.id);
        m_state.items.remove(i);
    }
    invalidateIndex();
    m_state.bins.removeIf([&](const Bin& b) { return doomed.contains(b.id); });
    for (const QString& itemId : std::as_const(removed))
        emit itemRemoved(itemId);
    emit binsChanged();
    touch();
    return removed;
}

bool MediaPool::setClipColor(const QString& id, const QString& colorId)
{
    const int i = indexOfItem(id);
    if (i < 0)
        return false;
    if (m_state.items[i].clipColor == colorId)
        return true;
    m_state.items[i].clipColor = colorId;
    emit itemChanged(id);
    touch();
    return true;
}

bool MediaPool::setFlags(const QString& id, quint32 flags)
{
    const int i = indexOfItem(id);
    if (i < 0)
        return false;
    if (m_state.items[i].flags == flags)
        return true;
    m_state.items[i].flags = flags;
    emit itemChanged(id);
    touch();
    return true;
}

bool MediaPool::setMetadata(const QString& id, MediaMetadata metadata)
{
    const int i = indexOfItem(id);
    if (i < 0)
        return false;
    QStringList keywords;
    for (const QString& k : std::as_const(metadata.keywords)) {
        const QString t = k.trimmed();
        if (!t.isEmpty() && !keywords.contains(t, Qt::CaseInsensitive))
            keywords.append(t);
    }
    metadata.keywords = keywords;
    metadata.rating = std::clamp(metadata.rating, 0, 5);
    if (m_state.items[i].metadata == metadata)
        return true;
    m_state.items[i].metadata = std::move(metadata);
    emit itemChanged(id);
    touch();
    return true;
}

const SmartBin* MediaPool::smartBin(const QString& id) const
{
    for (const SmartBin& b : m_state.smartBins) {
        if (b.id == id)
            return &b;
    }
    return nullptr;
}

QString MediaPool::addSmartBin(SmartBin bin)
{
    if (bin.id.isEmpty() || smartBin(bin.id))
        bin.id = newId();
    const QString id = bin.id;
    m_state.smartBins.append(std::move(bin));
    emit smartBinsChanged();
    touch();
    return id;
}

bool MediaPool::updateSmartBin(const SmartBin& bin)
{
    for (SmartBin& b : m_state.smartBins) {
        if (b.id != bin.id)
            continue;
        if (!(b == bin)) {
            b = bin;
            emit smartBinsChanged();
            touch();
        }
        return true;
    }
    return false;
}

bool MediaPool::removeSmartBin(const QString& id)
{
    const qsizetype removed
        = m_state.smartBins.removeIf([&](const SmartBin& b) { return b.id == id; });
    if (removed == 0)
        return false;
    emit smartBinsChanged();
    touch();
    return true;
}

void MediaPool::setState(MediaPoolState state)
{
    m_state = std::move(state);
    invalidateIndex();
    emit reset();
    touch();
}

} // namespace kk
