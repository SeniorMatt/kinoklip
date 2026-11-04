// SPDX-License-Identifier: GPL-3.0-or-later
// DroppedFileProbe implementation: QtConcurrent probes, results delivered on the GUI thread.
#include "ui/timeline/DroppedFileProbe.h"

#include "core/MediaPool.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace kk {

DroppedFileProbe::DroppedFileProbe(QObject* parent)
    : QObject(parent)
{ }

void DroppedFileProbe::request(const QStringList& paths)
{
    for (const QString& path : paths) {
        const QFileInfo fi(path);
        if (fi.isDir())
            continue; // folders go to the media pool only
        const QString absolute = fi.absoluteFilePath();
        const qint64 modified = MediaPool::modifiedMsOf(absolute);
        const auto it = m_entries.constFind(absolute);
        if (it != m_entries.cend() && it->modifiedMs == modified)
            continue;
        Entry entry;
        entry.modifiedMs = modified;
        entry.generation = ++m_generation;
        // Until the probe answers: a still of the default still duration (makeMediaClips()).
        entry.item.path = absolute;
        entry.item.name = fi.fileName();
        entry.item.info.valid = entry.item.info.hasVideo = entry.item.info.isStillImage = true;
        m_entries.insert(absolute, entry);

        auto* watcher = new QFutureWatcher<PreparedMedia>(this);
        const quint64 generation = entry.generation;
        connect(watcher, &QFutureWatcher<PreparedMedia>::finished, this,
            [this, watcher, absolute, generation] {
                watcher->deleteLater();
                const auto e = m_entries.find(absolute);
                if (e == m_entries.end() || e->generation != generation)
                    return;
                PreparedMedia prepared = watcher->result();
                if (prepared.ok) {
                    e->item = std::move(prepared.item);
                    if (e->item.name.isEmpty())
                        e->item.name = QFileInfo(absolute).fileName(); // as MediaPool::addItem()
                    e->state = State::Ready;
                } else {
                    e->state = State::Failed;
                }
                emit probed(absolute);
            });
        watcher->setFuture(QtConcurrent::run(&MediaPool::prepareFile, absolute));
    }
}

DroppedFileProbe::State DroppedFileProbe::state(const QString& path) const
{
    const auto it = m_entries.constFind(QFileInfo(path).absoluteFilePath());
    return it == m_entries.cend() ? State::Failed : it->state;
}

const MediaItem* DroppedFileProbe::item(const QString& path) const
{
    const auto it = m_entries.constFind(QFileInfo(path).absoluteFilePath());
    return it == m_entries.cend() || it->state == State::Failed ? nullptr : &it->item;
}

} // namespace kk
