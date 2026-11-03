// SPDX-License-Identifier: GPL-3.0-or-later
#include "pack/EmbeddedMedia.h"

#include "pack/Container.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kk::pack {

namespace {

const QString kMarker = QStringLiteral(".kinoklip/");

void setError(QString* error, const QString& text)
{
    if (error)
        *error = text;
}

/// The cached central directory of one container, valid while size and mtime are unchanged.
struct CachedIndex
{
    qint64 fileSize = -1;
    qint64 modifiedMs = 0;
    QHash<QString, EntryLocation> entries;
};

struct IndexCache
{
    QMutex mutex;
    QHash<QString, CachedIndex> containers;
};

IndexCache& indexCache()
{
    static IndexCache cache;
    return cache;
}

} // namespace

QString embeddedPath(const QString& containerPath, const QString& entryName)
{
    return containerPath + QLatin1Char('/') + entryName;
}

bool mayBeEmbeddedPath(const QString& path)
{
    return path.contains(kMarker, Qt::CaseInsensitive);
}

bool splitEmbeddedPath(const QString& path, QString* containerPath, QString* entryName)
{
    // Several ".kinoklip/" components are possible in theory ("x.kinoklip" folders): the first
    // prefix that is a regular file wins.
    for (qsizetype at = path.indexOf(kMarker, 0, Qt::CaseInsensitive); at >= 0;
         at = path.indexOf(kMarker, at + 1, Qt::CaseInsensitive)) {
        const QString prefix = path.left(at + kMarker.size() - 1);
        if (QFileInfo(prefix).isFile()) {
            if (containerPath)
                *containerPath = prefix;
            if (entryName)
                *entryName = path.mid(at + kMarker.size());
            return true;
        }
    }
    return false;
}

bool locateEntry(
    const QString& containerPath, const QString& entryName, EntryLocation* out, QString* error)
{
    const QFileInfo info(containerPath);
    if (!info.isFile()) {
        setError(error, QStringLiteral("Project file not found: %1").arg(containerPath));
        return false;
    }
    const qint64 size = info.size();
    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    IndexCache& cache = indexCache();
    QMutexLocker lock(&cache.mutex);
    CachedIndex& index = cache.containers[containerPath];
    if (index.fileSize != size || index.modifiedMs != mtime) {
        // Parse outside the lock would allow duplicate work only; parsing is a few ms, keep it
        // simple and hold the lock (other containers wait briefly).
        index = {};
        const std::unique_ptr<Container> c
            = Container::open(containerPath, Container::Mode::ReadOnly, error);
        if (!c) {
            cache.containers.remove(containerPath);
            return false;
        }
        for (const ContainerEntry& e : c->entries()) {
            const QDateTime when = e.modified();
            index.entries.insert(e.name,
                {containerPath, e.dataOffset, e.size, e.crc32,
                    when.isValid() ? when.toMSecsSinceEpoch() : 0});
        }
        index.fileSize = size;
        index.modifiedMs = mtime;
    }
    const auto it = index.entries.constFind(entryName);
    if (it == index.entries.cend()) {
        setError(error, QStringLiteral("%1 is not stored in %2").arg(entryName, info.fileName()));
        return false;
    }
    *out = *it;
    return true;
}

bool locateEmbedded(const QString& path, EntryLocation* out, QString* error)
{
    QString container;
    QString entry;
    if (!splitEmbeddedPath(path, &container, &entry)) {
        setError(error, QStringLiteral("File not found: %1").arg(path));
        return false;
    }
    return locateEntry(container, entry, out, error);
}

void invalidateIndexCache(const QString& containerPath)
{
    IndexCache& cache = indexCache();
    QMutexLocker lock(&cache.mutex);
    cache.containers.remove(containerPath);
}

// ---- RangeFile ---------------------------------------------------------------------------------

#if defined(Q_OS_UNIX)
struct RangeFile::Handle
{
    int fd = -1;
    ~Handle()
    {
        if (fd >= 0)
            ::close(fd);
    }
};
#else
struct RangeFile::Handle
{
    QFile file;
};
#endif

RangeFile::RangeFile() = default;
RangeFile::~RangeFile() = default;

bool RangeFile::isOpen() const
{
    return m_handle != nullptr;
}

bool RangeFile::open(const QString& filePath, qint64 offset, qint64 size, QString* error)
{
    auto handle = std::make_unique<Handle>();
    qint64 fileSize = 0;
#if defined(Q_OS_UNIX)
    handle->fd = ::open(QFile::encodeName(filePath).constData(), O_RDONLY | O_CLOEXEC);
    if (handle->fd < 0) {
        setError(error, QStringLiteral("Cannot open %1").arg(filePath));
        return false;
    }
    const off_t end = ::lseek(handle->fd, 0, SEEK_END);
    fileSize = end < 0 ? 0 : qint64(end);
#else
    handle->file.setFileName(filePath);
    if (!handle->file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        setError(error, QStringLiteral("Cannot open %1").arg(filePath));
        return false;
    }
    fileSize = handle->file.size();
#endif
    if (size < 0)
        size = fileSize - offset;
    if (offset < 0 || size < 0 || offset + size > fileSize) {
        setError(error, QStringLiteral("%1 is shorter than expected").arg(filePath));
        return false;
    }
    m_handle = std::move(handle);
    m_offset = offset;
    m_size = size;
    return true;
}

bool RangeFile::openPath(const QString& path, QString* error)
{
    if (mayBeEmbeddedPath(path) && !QFileInfo(path).isFile()) {
        EntryLocation where;
        if (!locateEmbedded(path, &where, error))
            return false;
        return open(where.containerPath, where.dataOffset, where.size, error);
    }
    return open(path, 0, -1, error);
}

qint64 RangeFile::read(qint64 pos, void* dst, qint64 n)
{
    if (!m_handle || pos < 0)
        return -1;
    n = std::min(n, m_size - pos);
    if (n <= 0)
        return 0;
    auto* out = static_cast<char*>(dst);
    qint64 done = 0;
    while (done < n) {
#if defined(Q_OS_UNIX)
        const ssize_t got
            = ::pread(m_handle->fd, out + done, size_t(n - done), off_t(m_offset + pos + done));
        if (got < 0 && errno == EINTR)
            continue;
#else
        if (!m_handle->file.seek(m_offset + pos + done))
            return -1;
        const qint64 got = m_handle->file.read(out + done, n - done);
#endif
        if (got < 0)
            return done > 0 ? done : -1;
        if (got == 0)
            break; // file truncated behind our back
        done += got;
    }
    return done;
}

// ---- RangeDevice -------------------------------------------------------------------------------

RangeDevice::RangeDevice(std::unique_ptr<RangeFile> file, QObject* parent)
    : QIODevice(parent)
    , m_file(std::move(file))
{ }

RangeDevice::~RangeDevice() = default;

std::unique_ptr<RangeDevice> RangeDevice::openPath(const QString& path, QString* error)
{
    auto file = std::make_unique<RangeFile>();
    if (!file->openPath(path, error))
        return {};
    auto device = std::make_unique<RangeDevice>(std::move(file));
    device->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    return device;
}

qint64 RangeDevice::size() const
{
    return m_file ? m_file->size() : 0;
}

qint64 RangeDevice::readData(char* data, qint64 maxSize)
{
    return m_file ? m_file->read(pos(), data, maxSize) : -1; // 0 at the end of the range
}

qint64 RangeDevice::writeData(const char*, qint64)
{
    return -1;
}

} // namespace kk::pack
