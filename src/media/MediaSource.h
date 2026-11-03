// SPDX-License-Identifier: GPL-3.0-or-later
// MediaSource — where the bytes of a media path are: a plain file, or an entry stored inside a
// single-file project ("/home/me/film.kinoklip/media/clip.mp4", see src/pack/EmbeddedMedia.h).
// Every place in the media engine that opens, stats or fingerprints a media file goes through
// this (FFmpeg custom I/O, Qt image readers, libheif, LibRaw, cache identities), so embedded
// media decode in place, with no extraction, exactly like files.
#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>

#include <memory>

class QImageReader;

namespace kk::media {

class MediaSource
{
public:
    /// Resolves @p path. Plain paths cost one stat; embedded paths one index-cache look-up.
    static MediaSource resolve(const QString& path);

    /// True if the bytes can be read (the file exists, or the container holds the entry).
    [[nodiscard]] bool exists() const { return m_exists; }
    /// True for an entry of a single-file project.
    [[nodiscard]] bool isEmbedded() const { return m_embedded; }
    [[nodiscard]] QString path() const { return m_path; }
    [[nodiscard]] qint64 size() const { return m_size; }
    /// Modification time (ms since the epoch); entries: the original file's ZIP time.
    [[nodiscard]] qint64 modifiedMs() const { return m_modifiedMs; }
    /// A string that changes whenever the content may have changed (cache keys): absolute path,
    /// time and size for files; container, entry, CRC and size for embedded media (stable
    /// across saves and Compact, which never change an entry's bytes).
    [[nodiscard]] QString identity() const { return m_identity; }

    /// A new, independent random-access device over the bytes (own file handle).
    std::unique_ptr<QIODevice> openDevice(QString* error = nullptr) const;
    /// All bytes in memory (libheif / LibRaw buffers); null on error or above @p limit bytes.
    QByteArray readAll(QString* error = nullptr, qint64 limit = 2LL << 30) const;

private:
    QString m_path;
    QString m_identity;
    qint64 m_size = 0;
    qint64 m_modifiedMs = 0;
    bool m_exists = false;
    bool m_embedded = false;
};

/// MediaSource::resolve(path).exists(), with the plain-file case kept to one stat.
bool mediaExists(const QString& path);

/// Points @p reader at @p path: a file name for plain files; for embedded media a device
/// (kept alive by @p holder, which must outlive the reader's use) and the format from the
/// file name extension.
void setImageReaderSource(
    QImageReader& reader, const QString& path, std::unique_ptr<QIODevice>& holder);

} // namespace kk::media
