// SPDX-License-Identifier: GPL-3.0-or-later
// DroppedFileProbe — probes the files of a file-manager drag on worker threads, so the
// timeline can show their ghost clips with the real length while the drag goes on.
#pragma once

#include "core/MediaItem.h"

#include <QHash>
#include <QObject>
#include <QStringList>

namespace kk {

/// Cache of MediaPool::prepareFile() results by absolute path (kept across drags, refreshed
/// when the file's modification time changes). request() starts the probes that are missing
/// and never blocks; probed() tells when one has finished.
class DroppedFileProbe : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Pending, ///< probing: item() is a placeholder (still, default still duration)
        Ready, ///< item() is what the import will create (no id, no bin)
        Failed ///< not importable: no ghost; the drop reports it like any import
    };

    explicit DroppedFileProbe(QObject* parent = nullptr);

    /// Starts probing every path of @p paths that is not cached yet (or changed on disk).
    void request(const QStringList& paths);
    /// State of @p path; Failed for a path that was never requested.
    [[nodiscard]] State state(const QString& path) const;
    /// The item for @p path (Pending: the placeholder); nullptr when Failed / unknown.
    [[nodiscard]] const MediaItem* item(const QString& path) const;

signals:
    void probed(const QString& path);

private:
    struct Entry
    {
        State state = State::Pending;
        MediaItem item;
        qint64 modifiedMs = 0;
        quint64 generation = 0; ///< ignores a stale worker result after a re-request
    };
    QHash<QString, Entry> m_entries;
    quint64 m_generation = 0;
};

} // namespace kk
