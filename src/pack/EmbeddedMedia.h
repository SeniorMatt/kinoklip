// SPDX-License-Identifier: GPL-3.0-or-later
// EmbeddedMedia — reading media stored inside a single-file project in place: the path
// convention for an entry ("<container>.kinoklip/media/clip.mp4"), a process-wide index cache
// that maps such a path to its byte range, and readers over that range with their own file
// handle (RangeFile for FFmpeg's custom I/O, RangeDevice for Qt's image readers).
#pragma once

#include <QIODevice>
#include <QString>

#include <cstdint>
#include <memory>

namespace kk::pack {

/// File name extension of single-file projects, without the dot.
inline constexpr char kContainerSuffix[] = "kinoklip";

/// Path of entry @p entryName inside @p containerPath, the way the rest of the application sees
/// it: the container acts like a folder, "/home/me/film.kinoklip/media/clip.mp4". Suffix and file
/// name of such a path are those of the media file, so routing by extension keeps working.
QString embeddedPath(const QString& containerPath, const QString& entryName);

/// Splits an embedded path into container and entry name. True only if a path component ends
/// in ".kinoklip" (any case) and that prefix is an existing regular file. Plain paths return
/// false after a single substring test (no file system access).
bool splitEmbeddedPath(const QString& path, QString* containerPath, QString* entryName);

/// Cheap string test: could @p path point into a container? (No file system access.)
bool mayBeEmbeddedPath(const QString& path);

/// Where an entry's bytes are.
struct EntryLocation
{
    QString containerPath;
    qint64 dataOffset = 0;
    qint64 size = 0;
    uint32_t crc32 = 0;
    qint64 modifiedMs = 0; ///< the entry's ZIP time (2 s resolution), ms since the epoch
};

/// Looks up an entry in the committed state of a container. Thread-safe. The parsed index is
/// cached per container and re-read when the file's size or modification time changes (a save
/// appends, Compact rewrites). Committed entries never move while the file exists, so a
/// location stays valid for readers that already opened it.
bool locateEntry(const QString& containerPath, const QString& entryName, EntryLocation* out,
    QString* error = nullptr);
/// locateEntry() for an embedded path.
bool locateEmbedded(const QString& path, EntryLocation* out, QString* error = nullptr);
/// Drops the cached index of @p containerPath (after this process wrote it).
void invalidateIndexCache(const QString& containerPath);

/// Random-access reader over [offset, offset + size) of a file with its own handle: any
/// number of RangeFiles may read one container from different threads at the same time, none
/// shares a seek position (POSIX pread). Not thread-safe itself (one per decoder).
class RangeFile
{
public:
    RangeFile();
    ~RangeFile();
    RangeFile(const RangeFile&) = delete;
    RangeFile& operator=(const RangeFile&) = delete;

    /// Opens a byte range of @p filePath; @p size < 0 = to the end of the file.
    bool open(const QString& filePath, qint64 offset, qint64 size, QString* error = nullptr);
    /// Opens an embedded path (its entry) or a plain file (the whole file).
    bool openPath(const QString& path, QString* error = nullptr);
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] qint64 size() const { return m_size; }
    /// Reads up to @p n bytes at range position @p pos. Returns the bytes read, 0 at the end of
    /// the range, -1 on an I/O error.
    qint64 read(qint64 pos, void* dst, qint64 n);

private:
    struct Handle;
    std::unique_ptr<Handle> m_handle;
    qint64 m_offset = 0;
    qint64 m_size = 0;
};

/// A read-only, random-access QIODevice over a RangeFile (Qt image readers, libheif, LibRaw).
class RangeDevice : public QIODevice
{
    Q_OBJECT
public:
    explicit RangeDevice(std::unique_ptr<RangeFile> file, QObject* parent = nullptr);
    ~RangeDevice() override;

    /// Opens @p path (embedded or plain) for reading; nullptr + @p error on failure.
    static std::unique_ptr<RangeDevice> openPath(const QString& path, QString* error = nullptr);

    [[nodiscard]] bool isSequential() const override { return false; }
    [[nodiscard]] qint64 size() const override;

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 maxSize) override;

private:
    std::unique_ptr<RangeFile> m_file;
};

} // namespace kk::pack
