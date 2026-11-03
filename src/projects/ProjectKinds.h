// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectKinds — the kinds of project file the library lists, each with its metadata reader.
#pragma once

#include "core/Rational.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>

namespace kk {

/// What the Project Manager shows about a project without opening it. Filled by a kind's
/// metadata reader (on a worker thread) and cached by ProjectLibrary.
struct ProjectMetadata
{
    QString name; ///< project name stored in the file (the card shows the file name)
    int width = 0; ///< project resolution in pixels (0 = unknown)
    int height = 0;
    Rational fps {0, 1}; ///< project frame rate ({0, 1} = unknown)
    int64_t durationFrames = 0; ///< length of the active timeline, in timelineFps frames
    Rational timelineFps {0, 1}; ///< frame rate of the active timeline
    int timelineCount = 0; ///< regular and compound timelines
    int mediaCount = 0; ///< media files in the media pool
    int missingMediaCount = 0; ///< of those, files that do not exist (offline)
    /// Poster frame: the media file and source time (µs) shown by the active timeline at its
    /// saved playhead (or its first video clip). Empty if the timeline shows no media file.
    QString posterMediaPath;
    int64_t posterTimeUs = 0;
    /// Non-empty if the file could not be read; the card shows it instead of the details.
    QString error;

    [[nodiscard]] bool isValid() const { return error.isEmpty(); }

    friend bool operator==(const ProjectMetadata&, const ProjectMetadata&) = default;
};

/// Where a project keeps its media (owner decision, plan-v4 part 2).
enum class ProjectStorage {
    Linked, ///< `.kkproj`: the project file references media where it is
    SingleFile ///< `.kinoklip`: one file holding the project data and copies of the media
};

/// One kind of project file: the linked `.kkproj` (JSON) and the self-contained single-file
/// `.kinoklip` (plan-v4 part 2). The library lists every file whose extension belongs to a
/// registered kind and asks that kind for the metadata.
///
/// Integration point for the single-file container (kkpack branch): fill `readMetadata`,
/// `writeName` and `create` of the "kinoklip" kind (registerKind() replaces the placeholder
/// registered here), and opening / saving in ProjectController. Until then the placeholder
/// lists `.kinoklip` files with a "not supported yet" error card and cannot create projects.
struct ProjectKind
{
    QString id; ///< stable id ("kkproj", "kinoklip")
    QString displayName; ///< "Linked project"
    QString badge; ///< short card badge ("Linked", "Single file")
    ProjectStorage storage = ProjectStorage::Linked;
    QStringList extensions; ///< lower-case, without the dot ("kkproj")
    /// Reads the metadata of the file at the absolute path. Must be thread-safe (runs on the
    /// library's worker threads) and never throw; an unreadable file sets `error`.
    std::function<ProjectMetadata(const QString& path)> readMetadata;
    /// Updates the project name stored inside the file after a rename (may be null: the
    /// file name is then the only name). Returns false and sets the error on failure.
    std::function<bool(const QString& path, const QString& name, QString* error)> writeName;
    /// Factory: writes a new, empty project named @p name at @p path (extensions.first()).
    /// Null = this build cannot create projects of this kind (canCreate() false).
    std::function<bool(const QString& path, const QString& name, QString* error)> create;

    [[nodiscard]] bool canCreate() const { return bool(create); }
};

namespace projectkinds {

/// Every registered kind; the built-in `.kkproj` kind is always first. GUI thread for
/// registration; reading the list is safe from any thread once start-up is over.
[[nodiscard]] const QVector<ProjectKind>& all();
/// Adds a kind (at start-up, before any library scans). A kind with an existing id replaces it.
void registerKind(const ProjectKind& kind);
/// The kind with id @p id, or nullptr.
[[nodiscard]] const ProjectKind* byId(const QString& id);
/// The first kind with storage @p storage, or nullptr.
[[nodiscard]] const ProjectKind* byStorage(ProjectStorage storage);
/// The kind of the file @p path (by extension, case-insensitive), or nullptr.
[[nodiscard]] const ProjectKind* forPath(const QString& path);
/// Name filters for QDir / file dialogs ("*.kkproj", ...).
[[nodiscard]] QStringList nameFilters();
/// The `.kkproj` metadata reader (JSON; tolerant of every format version Project::load()
/// reads, refuses newer ones with an error).
[[nodiscard]] ProjectMetadata readKkprojMetadata(const QString& path);
/// Writes a new empty `.kkproj` project (Project::save()); the linked kind's factory.
bool createKkproj(const QString& path, const QString& name, QString* error);
/// Rewrites the "project" / "name" field of a `.kkproj` file (atomically, QSaveFile).
bool writeKkprojName(const QString& path, const QString& name, QString* error);

} // namespace projectkinds

} // namespace kk
