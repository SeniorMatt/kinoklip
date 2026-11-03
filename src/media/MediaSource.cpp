// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/MediaSource.h"

#include "pack/EmbeddedMedia.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>

namespace kk::media {

MediaSource MediaSource::resolve(const QString& path)
{
    MediaSource s;
    s.m_path = path;
    const QFileInfo info(path);
    if (info.isFile()) {
        s.m_exists = true;
        s.m_size = info.size();
        s.m_modifiedMs = info.lastModified().toMSecsSinceEpoch();
        s.m_identity = info.absoluteFilePath() + QLatin1Char('|') + QString::number(s.m_modifiedMs)
            + QLatin1Char('|') + QString::number(s.m_size);
        return s;
    }
    pack::EntryLocation where;
    if (pack::mayBeEmbeddedPath(path) && pack::locateEmbedded(path, &where)) {
        s.m_exists = true;
        s.m_embedded = true;
        s.m_size = where.size;
        s.m_modifiedMs = where.modifiedMs;
        s.m_identity = QFileInfo(path).absoluteFilePath() + QLatin1String("|crc")
            + QString::number(where.crc32, 16) + QLatin1Char('|') + QString::number(s.m_size);
    }
    return s;
}

std::unique_ptr<QIODevice> MediaSource::openDevice(QString* error) const
{
    if (!m_embedded) {
        auto file = std::make_unique<QFile>(m_path);
        if (!file->open(QIODevice::ReadOnly)) {
            if (error)
                *error = file->errorString();
            return {};
        }
        return file;
    }
    return pack::RangeDevice::openPath(m_path, error);
}

QByteArray MediaSource::readAll(QString* error, qint64 limit) const
{
    if (m_size > limit) {
        if (error)
            *error = QStringLiteral("File too large to load into memory");
        return {};
    }
    const std::unique_ptr<QIODevice> device = openDevice(error);
    if (!device)
        return {};
    QByteArray data = device->readAll();
    if (data.size() != m_size) {
        if (error)
            *error = QStringLiteral("Cannot read %1").arg(m_path);
        return {};
    }
    return data;
}

bool mediaExists(const QString& path)
{
    if (QFileInfo(path).isFile())
        return true;
    return pack::mayBeEmbeddedPath(path) && MediaSource::resolve(path).exists();
}

void setImageReaderSource(
    QImageReader& reader, const QString& path, std::unique_ptr<QIODevice>& holder)
{
    if (!pack::mayBeEmbeddedPath(path) || QFileInfo(path).isFile()) {
        reader.setFileName(path);
        return;
    }
    holder = MediaSource::resolve(path).openDevice();
    if (!holder) {
        reader.setFileName(path); // fails with "file not found", like a missing file
        return;
    }
    // The format as Qt names it when reading the file by name: detected from the content
    // ("jpeg", not the suffix "jpg"); the suffix only for formats without a signature (TGA).
    QByteArray format = QImageReader::imageFormat(holder.get());
    if (format.isEmpty())
        format = QFileInfo(path).suffix().toLower().toLatin1();
    holder->seek(0);
    reader.setDevice(holder.get());
    reader.setFormat(format);
}

} // namespace kk::media
