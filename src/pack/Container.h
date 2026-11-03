// SPDX-License-Identifier: GPL-3.0-or-later
// Container — the ZIP-based single-file project container (docs/single-file-project.md):
// stored (uncompressed) ZIP64 entries, append-only saves, crash-safe commits with two header
// slots in the first entry. This is the low-level layer: entries and bytes. The project-level
// operations (document, manifest, pack, unpack, compact, verify) are in ProjectPackage.h.
#pragma once

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>

namespace kk::pack {

/// Progress of a long operation: bytes done of bytes total (total may grow while discovering
/// work). Return false to cancel; the operation then stops at the next chunk (8 MiB).
using Progress = std::function<bool(qint64 done, qint64 total)>;

/// Outcome of an operation that can be cancelled.
enum class Status {
    Ok,
    Failed, ///< see the error message
    Cancelled ///< the Progress callback returned false; nothing was committed
};

/// One live entry of the container (an entry of the current central directory).
struct ContainerEntry
{
    QString name; ///< UTF-8 entry name, '/' separated, e.g. "media/clip.mp4"
    qint64 headerOffset = 0; ///< file offset of the ZIP local file header
    qint64 dataOffset = 0; ///< file offset of the first data byte (stored: the file itself)
    qint64 size = 0; ///< data size in bytes (stored: compressed == uncompressed)
    uint32_t crc32 = 0; ///< CRC-32 of the data
    uint16_t dosTime = 0; ///< ZIP modification time (local time, 2 s resolution)
    uint16_t dosDate = 0;

    /// Bytes this entry occupies in the file: local header + data.
    [[nodiscard]] qint64 footprint() const { return dataOffset - headerOffset + size; }
    /// The ZIP modification time as a local QDateTime (invalid if not set).
    [[nodiscard]] QDateTime modified() const;
};

/// How open() found the committed state (Verify reports it; see the spec's recovery section).
enum class Recovery {
    Clean, ///< the file ends with a valid end record: the normal case
    HeaderSlot, ///< the tail was incomplete (crash during a save); the header slot was used
    Scan, ///< the header slots were unusable too; found by scanning backwards
};

/// A container file. Not thread-safe; readers in other threads use RangeFile (EmbeddedMedia.h)
/// with their own file handles. One writer per file at a time (the caller locks).
class Container
{
public:
    enum class Mode {
        ReadOnly,
        ReadWrite
    };

    /// The media type stored in the first entry ("mimetype", like ODF / EPUB).
    static const QByteArray& mimeType();

    /// Creates (truncates) @p path as an empty container and commits it (generation 1).
    static std::unique_ptr<Container> create(const QString& path, QString* error);
    /// Opens an existing container and finds its last committed state. A torn tail left by a
    /// crash during a save is ignored (ReadWrite: cut off at the first write).
    static std::unique_ptr<Container> open(const QString& path, Mode mode, QString* error);
    /// True when the first bytes of @p path look like a container (cheap: one 64-byte read).
    static bool looksLikeContainer(const QString& path);

    ~Container();
    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    [[nodiscard]] QString path() const { return m_path; }
    [[nodiscard]] Mode mode() const { return m_mode; }
    /// Live entries in file order (pending, uncommitted ones included).
    [[nodiscard]] const QVector<ContainerEntry>& entries() const { return m_entries; }
    /// The live entry called @p name, or nullptr.
    [[nodiscard]] const ContainerEntry* find(const QString& name) const;
    /// Commit counter; grows by one per commit().
    [[nodiscard]] quint64 generation() const { return m_generation; }
    /// File size of the committed state (bytes after it are an uncommitted tail).
    [[nodiscard]] qint64 committedSize() const { return m_committedEnd; }
    /// Bytes after the committed state found at open() (a torn save), 0 normally.
    [[nodiscard]] qint64 ignoredTailBytes() const { return m_ignoredTail; }
    [[nodiscard]] Recovery recovery() const { return m_recovery; }
    /// Size the file would have when rewritten with only the live entries (Compact preview).
    [[nodiscard]] qint64 liveSize() const;

    /// Reads a whole entry into memory and checks its CRC (documents, manifests, small stills).
    QByteArray read(const ContainerEntry& entry, QString* error) const;
    /// Reads @p size bytes at file offset @p offset (through this object's handle).
    bool readAt(qint64 offset, char* dst, qint64 size) const;

    // ---- Writing (Mode::ReadWrite). Added entries become visible to other readers only after
    // commit(); an added name that already exists replaces the old entry (its bytes stay in
    // the file until Compact). ----

    /// Appends an entry with the bytes @p data.
    bool addData(
        const QString& name, const QByteArray& data, const QDateTime& modified, QString* error);
    /// Appends the file @p sourcePath (or the byte range [sourceOffset, +sourceSize) of it when
    /// sourceSize >= 0) unchanged, computing its CRC while copying. Runs of zero bytes become
    /// holes (sparse files stay sparse). @p crcOut receives the CRC.
    Status addFile(const QString& name, const QString& sourcePath, const QDateTime& modified,
        const Progress& progress, QString* error, qint64 sourceOffset = 0, qint64 sourceSize = -1,
        uint32_t* crcOut = nullptr, qint64 progressBase = 0, qint64 progressTotal = 0);
    /// Removes @p name from the index of the next commit (the bytes stay until Compact).
    bool remove(const QString& name);
    /// True when entries were added or removed since the last commit.
    [[nodiscard]] bool hasPendingChanges() const { return m_pending; }
    /// Makes the pending changes durable: data fsync, new central directory and end records,
    /// fsync, then the header slot, fsync. A crash at any point keeps the previous state.
    bool commit(QString* error);
    /// Forgets the pending changes and cuts the file back to the committed state.
    void rollback();

    /// Test hook: commit() stops after writing this many bytes of its records (simulates a
    /// crash in the middle of a save; the object is unusable afterwards). -1 = off.
    void setCrashAfterBytesForTest(qint64 bytes) { m_crashAfter = bytes; }

private:
    Container() = default;
    struct State;
    bool loadState(qint64 eocdPos, quint64 expectedGeneration, State* state) const;
    bool findCommittedState(QString* error);
    bool prepareWrite(QString* error);
    bool writeAt(qint64 offset, const QByteArray& bytes, QString* error);
    bool writeHeaderSlot(quint64 generation, qint64 eocdPos, qint64 end, QString* error);
    void putEntry(const ContainerEntry& entry);

    QString m_path;
    Mode m_mode = Mode::ReadOnly;
    mutable QFile m_file;
    QVector<ContainerEntry> m_entries;
    QHash<QString, int> m_index; ///< name -> position in m_entries
    QVector<ContainerEntry> m_committedEntries; ///< for rollback()
    quint64 m_generation = 0;
    qint64 m_committedEnd = 0;
    qint64 m_writeEnd = 0; ///< where the next appended entry starts
    qint64 m_ignoredTail = 0;
    bool m_tailCut = false; ///< the uncommitted tail was truncated (first write)
    bool m_pending = false;
    Recovery m_recovery = Recovery::Clean;
    qint64 m_crashAfter = -1;
};

/// Entry names are relative, '/' separated, without "." / ".." components or backslashes.
bool isValidEntryName(const QString& name);

/// fdatasync() / FlushFileBuffers() of an open file. Returns false on I/O error.
bool syncFile(QFile& file);
/// Makes a rename inside @p dir durable (fsync of the directory; no-op where unsupported).
void syncDirectory(const QString& dir);

} // namespace kk::pack
