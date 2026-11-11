// SPDX-License-Identifier: GPL-3.0-or-later
// MatteCache — mattes kept on disk per media file, quality and source frame (spec 78), so a clip
// is cut out once, not again after a restart. A folder per (file path, size, modification time,
// model, quality); inside, one small greyscale PNG per source time in milliseconds.
#pragma once

#include <QImage>
#include <QString>

#include <cstdint>

namespace kk::matting {

class MatteCache
{
public:
    explicit MatteCache(QString root);

    /// Folder name for a media file: changes when the file, the model or the quality changes.
    [[nodiscard]] static QString folderKey(const QString& mediaPath, qint64 fileSize,
        qint64 modifiedMs, const QString& modelId, int quality);

    [[nodiscard]] QImage load(const QString& folderKey, int64_t ms) const;
    void store(const QString& folderKey, int64_t ms, const QImage& matte) const;
    [[nodiscard]] bool contains(const QString& folderKey, int64_t ms) const;
    /// Times (ms) stored for a media file, ascending.
    [[nodiscard]] QList<int64_t> times(const QString& folderKey) const;

    /// Total size of the cache on disk, and its removal (Preferences).
    [[nodiscard]] qint64 sizeBytes() const;
    bool clear() const;
    /// Deletes the oldest media folders until the cache is below @p limitBytes.
    void trim(qint64 limitBytes) const;

    [[nodiscard]] const QString& root() const { return m_root; }

private:
    [[nodiscard]] QString filePath(const QString& folderKey, int64_t ms) const;
    QString m_root;
};

} // namespace kk::matting
