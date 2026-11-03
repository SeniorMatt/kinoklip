// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectLibrary — the folder of projects behind the Project Manager: listing, folders,
// metadata (read on worker threads, cached on disk), posters and the file operations.
#pragma once

#include "projects/ProjectKinds.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVector>

#include <functional>
#include <optional>

namespace kk {

/// One row of a library listing: a project file or a folder.
struct ProjectEntry
{
    QString path; ///< absolute path
    QString name; ///< file name without the extension (folders: the folder name)
    qint64 modifiedMs = 0; ///< last modification, ms since the epoch (UTC)
    qint64 size = 0; ///< file size in bytes (0 for folders)
    bool isFolder = false;
    QString kindId; ///< ProjectKind::id of a project file
    bool exists = true; ///< false for a recent project whose file is gone

    friend bool operator==(const ProjectEntry&, const ProjectEntry&) = default;
};

/// The project library: a folder (default `~/Videos/Kinoklip Projects`, Preferences > Project
/// Save and Load) whose sub-folders organise the projects. The file system is the only truth:
/// a folder of the library is a folder on disk, a project is a file of a registered
/// ProjectKind, so the user may also organise the library with a file manager.
///
/// **Metadata** (resolution, duration, poster frame, missing media) is read lazily: metadata()
/// returns the cached value or schedules the kind's reader on a private thread pool and emits
/// metadataReady() later. Results are kept in memory and in a sidecar cache
/// (`<cache>/metadata.json`, keyed by path + modification time + size), so a library of
/// hundreds of projects lists instantly on the next start without opening a single project.
/// **Posters** are images cached as `<cache>/posters/<sha1(path)>-<mtime>.jpg`; poster()
/// loads them on the worker threads too (posterReady()), storePoster() adds one.
///
/// **Operations** (create, rename, duplicate, move, trash, import, export) return the new
/// path or false with a user-readable error. The project that is open in the editor
/// (setOpenProjectPath()) cannot be deleted, renamed or moved.
///
/// Thread-safety: GUI thread only (workers only read files and post their results back).
class ProjectLibrary : public QObject
{
    Q_OBJECT
public:
    /// A library rooted at @p root (created if missing). @p cacheDir holds the metadata and
    /// poster caches; empty = `<QStandardPaths::CacheLocation>/project-library`.
    explicit ProjectLibrary(
        const QString& root, const QString& cacheDir = {}, QObject* parent = nullptr);
    /// Waits for running metadata jobs and writes the metadata cache.
    ~ProjectLibrary() override;

    /// Absolute library folder.
    [[nodiscard]] QString root() const { return m_root; }
    /// Folder of the metadata / poster caches.
    [[nodiscard]] QString cacheDirectory() const { return m_cacheDir; }
    /// True if @p path is the library folder or inside it.
    [[nodiscard]] bool contains(const QString& path) const;

    // ---- Listing ----

    /// Folders, then project files of @p folder (absolute; empty = root), unsorted. Hidden
    /// files and files of unknown kinds are skipped. Fast: one directory read, no file opened.
    [[nodiscard]] QVector<ProjectEntry> list(const QString& folder = {}) const;
    /// Entry for the file or folder at @p path (exists = false if it is gone).
    [[nodiscard]] static ProjectEntry entryFor(const QString& path);
    /// Entries for @p paths (the Recent list), in that order; missing files included.
    [[nodiscard]] static QVector<ProjectEntry> entriesFor(const QStringList& paths);

    // ---- Metadata and posters ----

    /// Metadata of @p entry when known for its current modification time and size; otherwise
    /// nullopt and the reader is scheduled (metadataReady(path) follows). Folders: nullopt.
    std::optional<ProjectMetadata> metadata(const ProjectEntry& entry);
    /// Reads the metadata synchronously (and caches it). For tests and one-off callers.
    ProjectMetadata metadataNow(const ProjectEntry& entry);
    /// State of a poster request.
    enum class PosterState {
        Ready, ///< poster() returned the image
        Loading, ///< being read from the cache; posterReady(path) follows
        Absent ///< nothing cached for this version of the file: the caller may make one
    };
    /// The cached poster of @p entry (null unless @p state is Ready).
    QImage poster(const ProjectEntry& entry, PosterState* state = nullptr);
    /// Caches @p image as the poster of @p entry (memory now, disk on a worker thread).
    void storePoster(const ProjectEntry& entry, const QImage& image);
    /// Writes the metadata cache now (it is also written a moment after every change).
    void flushCache();
    /// Blocks until every scheduled metadata / poster job has finished and its result has
    /// been delivered (tests, benchmarks).
    void waitForIdle();
    /// Number of metadata reads that went to the project file (not the cache); for tests.
    [[nodiscard]] int metadataReads() const { return m_reads; }

    // ---- The open project ----

    /// The project open in the editor (absolute path, empty = none / never saved).
    void setOpenProjectPath(const QString& path);
    [[nodiscard]] QString openProjectPath() const { return m_openPath; }
    /// True if @p path is the open project, or a folder containing it.
    [[nodiscard]] bool isOpenOrContainsOpen(const QString& path) const;

    // ---- Operations ----

    /// Checks a project or folder name typed by the user (not empty, no '/', not "." / "..").
    [[nodiscard]] static bool isValidName(const QString& name, QString* error = nullptr);
    /// A free file name "base.ext", "base 2.ext", ... in @p folder (@p ext without the dot;
    /// empty for folders).
    [[nodiscard]] static QString uniquePath(
        const QString& folder, const QString& base, const QString& ext);
    /// New empty project named @p name in @p folder (empty = root) of the kind @p kindId
    /// (ProjectKind::id; its factory writes it right away, so it exists in the library like any
    /// other). Returns its path, or empty with @p error (also when the kind cannot create).
    QString createProject(const QString& folder, const QString& name,
        const QString& kindId = QStringLiteral("kkproj"), QString* error = nullptr);
    /// New sub-folder of @p folder. Returns its path, or empty with @p error.
    QString createFolder(const QString& folder, const QString& name, QString* error = nullptr);
    /// Renames a project (file and the name stored inside it) or a folder. Returns the new
    /// path, or empty with @p error (name taken, open project, I/O error).
    QString rename(const QString& path, const QString& newName, QString* error = nullptr);
    /// Copies a project next to itself as "<name> copy" (the stored name follows). Returns the
    /// new path, or empty with @p error.
    QString duplicate(const QString& path, QString* error = nullptr);
    /// Moves a project or folder into @p folder (a folder never into itself). Returns the new
    /// path (unchanged if already there), or empty with @p error.
    QString move(const QString& path, const QString& folder, QString* error = nullptr);
    /// Moves a project or folder to the system trash (freedesktop trash; refused for the open
    /// project and folders containing it). Cached metadata and posters are dropped.
    bool moveToTrash(const QString& path, QString* error = nullptr);
    /// Copies the project file @p source (anywhere) into @p folder of the library. Returns the
    /// new path, or empty with @p error (unknown kind, unreadable).
    QString importProject(const QString& source, const QString& folder, QString* error = nullptr);
    /// Saves a copy of @p path as @p target (overwriting it). False with @p error on failure.
    /// A different kind converts: a linked project exported as `.kinoklip` is packed (every
    /// media file copied in), a single-file project exported as `.kkproj` is unpacked (the
    /// media written to `<target folder>/media/...`, never overwriting). @p progress (bytes
    /// done, total; false = cancel) reports the copy; a cancelled export sets
    /// @p cancelled and leaves nothing behind. Touches no member: may run on a worker thread.
    bool exportProject(const QString& path, const QString& target, QString* error = nullptr,
        const std::function<bool(qint64, qint64)>& progress = {}, bool* cancelled = nullptr,
        QStringList* keptLinked = nullptr) const;

signals:
    /// metadata() of @p path is now available.
    void metadataReady(const QString& path);
    /// poster() of @p path is now available (or known to be absent).
    void posterReady(const QString& path);
    /// The library's files changed through one of the operations above (views refresh).
    void changed();

private:
    struct CachedMeta
    {
        qint64 modifiedMs = 0;
        qint64 size = 0;
        ProjectMetadata meta;
    };
    struct CachedPoster
    {
        qint64 modifiedMs = 0;
        PosterState state = PosterState::Absent;
        QImage image;
    };

    [[nodiscard]] QString posterFile(const QString& path, qint64 modifiedMs) const;
    void loadCache();
    void scheduleFlush();
    void forget(const QString& path); ///< drops cached data of @p path (and below a folder)
    bool refuseOpen(const QString& path, const char* what, QString* error) const;

    QString m_root;
    QString m_cacheDir;
    QString m_openPath;
    QHash<QString, CachedMeta> m_meta;
    QHash<QString, CachedPoster> m_posters;
    QSet<QString> m_pendingMeta; ///< paths whose reader runs
    QThreadPool m_pool;
    QTimer m_flushTimer;
    bool m_cacheDirty = false;
    int m_reads = 0;
    int m_inFlight = 0; ///< jobs whose result has not been delivered yet
};

} // namespace kk
