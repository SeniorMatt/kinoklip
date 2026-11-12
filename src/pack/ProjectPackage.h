// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectPackage — a single-file project: the Container plus its project document, manifest
// and embedded media, and the operations on whole projects (Pack, Unpack, Add Media, Compact,
// Verify). Library only: menus, dialogs and the Project Manager call these (integration step,
// see docs/single-file-project.md). Every long operation reports progress and can be cancelled.
#pragma once

#include "pack/Container.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace kk::pack {

/// Entry names inside the container.
inline constexpr char kDocumentEntry[] = "project.kkproj"; ///< the project document (JSON)
inline constexpr char kManifestEntry[] = "manifest.json"; ///< what is stored, see MediaRecord
inline constexpr char kMediaPrefix[] = "media/"; ///< embedded media files live below this
inline constexpr int kManifestVersion = 1;

/// One embedded media file as listed in the manifest.
struct MediaRecord
{
    QString entry; ///< "media/<id>/clip.mp4"
    /// Recognises a re-import of the same file without storing where it came from:
    /// hex SHA-256 of (container salt, absolute source path, size, modification time). The salt
    /// is random per container, so the same file in two containers gives unrelated keys.
    QString sourceKey;
    qint64 size = 0;
    qint64 modifiedMs = 0; ///< modification time of the original file (ms since the epoch, UTC)
    uint32_t crc32 = 0;
    qint64 addedMs = 0; ///< when it was copied in
};

/// True if @p path ends in ".kinoklip" (any case): the file is a single-file project.
bool isContainerFile(const QString& path);

// ---- Document forms (docs/single-file-project.md, "Media references") ------------------------

/// Stored form -> runtime form: every media pool item with an "embedded" entry gets the
/// embedded path ("<container>/media/clip.mp4") as its "path" (relativePath dropped), so the
/// normal project loader and the media engine see an ordinary path.
QByteArray documentToRuntime(const QByteArray& stored, const QString& containerPath);
/// Runtime form -> stored form: items whose path lies inside @p containerPath get
/// "embedded" = entry name and path / relativePath = the entry name (relative, so an archive
/// extracted with any unzip tool opens as a normal linked project).
QByteArray documentToStorage(const QByteArray& runtime, const QString& containerPath);
/// Entry names referenced by the "embedded" fields of a stored document.
QStringList embeddedEntries(const QByteArray& storedDocument);

/// An open single-file project.
class ProjectPackage
{
public:
    /// Creates a new, empty single-file project at @p path (overwrites).
    static std::unique_ptr<ProjectPackage> create(const QString& path, QString* error);
    /// Opens @p path. @p writable = saving and importing allowed (the caller holds the lock).
    static std::unique_ptr<ProjectPackage> open(const QString& path, bool writable, QString* error);
    ~ProjectPackage();

    [[nodiscard]] QString path() const { return m_path; }
    [[nodiscard]] Container& container() { return *m_container; }
    [[nodiscard]] const Container& container() const { return *m_container; }
    [[nodiscard]] const QVector<MediaRecord>& media() const { return m_media; }
    [[nodiscard]] const MediaRecord* record(const QString& entry) const;

    /// True when a project document has been saved into the file.
    [[nodiscard]] bool hasDocument() const;
    /// The project document in runtime form (embedded paths), empty + @p error on failure.
    QByteArray document(QString* error = nullptr) const;
    /// The document as stored (entry names).
    QByteArray storedDocument(QString* error = nullptr) const;

    /// Saves a project document given in runtime form: appends the document and the manifest
    /// and commits. Media bytes are not touched. A crash keeps the previous saved state.
    bool saveDocument(const QByteArray& runtimeDocument, QString* error);
    /// Same for a document already in stored form.
    bool saveStoredDocument(const QByteArray& storedDocument, QString* error);

    /// Result of adding one file.
    struct Added
    {
        QString source;
        QString entry; ///< entry name inside this project
        QString path; ///< embedded path to use in the project (media pool item path)
        bool duplicate = false; ///< the file was already stored: nothing copied
    };
    /// Copies media files into the project (import into a self-contained project) and commits.
    /// A file already stored (same original path, size and time, or identical content) is not
    /// copied twice. @p sources may be plain paths or embedded paths of another container.
    /// Cancelled or failed: nothing of this call is kept.
    Status addMedia(const QStringList& sources, QVector<Added>* added, const Progress& progress,
        QString* error);

    /// Replaces the manifest's media records (written with the next save; Compact uses it).
    void setMediaRecords(const QVector<MediaRecord>& records, const QByteArray& salt)
    {
        m_media = records;
        m_salt = salt;
    }
    /// The manifest's random salt (hex) for MediaRecord::sourceKey.
    [[nodiscard]] QByteArray salt() const { return m_salt; }
    /// sourceKey of a file at @p absolutePath with @p size and @p modifiedMs in this container.
    [[nodiscard]] QString sourceKey(
        const QString& absolutePath, qint64 size, qint64 modifiedMs) const;
    /// Writes the manifest alone and commits (a project without a document yet).
    bool saveManifest(QString* error);

    /// Size after Compact (only media referenced by the document, latest document only).
    [[nodiscard]] qint64 compactedSizeEstimate() const;
    /// Media entries the document does not reference (Compact removes them).
    [[nodiscard]] QStringList unusedMedia() const;

private:
    ProjectPackage() = default;
    bool loadManifest(QString* error);
    QByteArray manifestJson() const;
    QString uniqueEntryName(const QString& fileName) const;

    QString m_path;
    std::unique_ptr<Container> m_container;
    QVector<MediaRecord> m_media;
    QByteArray m_salt; ///< 32 hex digits, created with the container
};

// ---- Operations on files ------------------------------------------------------------------

struct PackResult
{
    Status status = Status::Failed;
    QString error;
    int embedded = 0; ///< media files copied in (duplicates counted once)
    QStringList skipped; ///< media left linked: offline files, image sequences (with reason)
    qint64 bytesCopied = 0;
};
/// Pack: linked project file (.kkproj) + every media file it uses -> one single-file project
/// at @p containerPath (written to a temporary file next to it, renamed when complete).
PackResult packProject(
    const QString& projectFile, const QString& containerPath, const Progress& progress = {});

struct UnpackResult
{
    Status status = Status::Failed;
    QString error;
    QString projectFile; ///< the linked project written into the folder
    int extracted = 0;
};
/// Unpack: writes the media to @p folder/media/... and a linked project file
/// @p folder/<name>.kkproj that refers to them. Existing files are never overwritten.
UnpackResult unpackProject(
    const QString& containerPath, const QString& folder, const Progress& progress = {});

struct ExtractResult
{
    Status status = Status::Failed;
    QString error;
};
/// Save a Copy: writes the stored bytes of the media entry @p entryName of @p containerPath
/// (not a re-encode) to the file @p target, through "<target>.part" renamed when the CRC
/// matched; an existing @p target is replaced only then. The container is only read. Cancelled
/// or failed: no @p target change and no ".part" left.
ExtractResult extractMedia(const QString& containerPath, const QString& entryName,
    const QString& target, const Progress& progress = {});

struct CompactResult
{
    Status status = Status::Failed;
    QString error;
    qint64 sizeBefore = 0;
    qint64 sizeAfter = 0;
    QStringList removedMedia;
};
/// Compact: rewrites the file without media the document does not reference and without old
/// document versions (temporary file in the same folder, fsync, atomic rename). Readers that
/// still have the old file open keep reading it (POSIX); reopen to see the new one.
CompactResult compactProject(const QString& containerPath, const Progress& progress = {});

struct VerifyResult
{
    bool ok = false; ///< no problems (notes are informational)
    QStringList problems; ///< damage: CRC mismatches, missing entries, overlapping data...
    QStringList notes; ///< recovered after a crash, unused media, ...
    int entries = 0;
    qint64 checkedBytes = 0;
    Recovery recovery = Recovery::Clean;
    qint64 ignoredTailBytes = 0;
};
/// Verify: checks the index, the document and manifest and (with @p checkData) the CRC of
/// every entry's bytes.
VerifyResult verifyContainer(
    const QString& containerPath, bool checkData = true, const Progress& progress = {});

/// After a crash during a save: cuts off the torn tail and writes a fresh end record, so
/// standard ZIP tools read the file again. No-op for a clean file.
bool repairContainer(const QString& containerPath, QString* error = nullptr);

} // namespace kk::pack
