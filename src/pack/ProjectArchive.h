// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectArchive — File > Export Project Archive / Restore Project Archive (Resolve: a ".dra"
// folder holding the project and all its media, optionally the proxy media and the render
// cache). Library only: the File menu, the command line and the MCP server call these.
//
// An archive is a folder "<name>.dra":
//   archive.json        what it holds (format "kinoklip-archive", version, project file name, counts)
//   <name>.kkproj       the project, a linked project whose media paths are relative to the folder
//   Media/<n>/<file>    every media file the project uses (image sequences: all frames)
//   Luts/<n>/<file>     the LUT files of grade nodes and input LUTs
//   Proxy/<n>/<file>    proxy media (option), Optimized/<n>/<file> optimized media (option)
//   Cache/<file>        render cache chunks (option; the caller names the files)
#pragma once

#include "pack/Container.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace kk::pack {

inline constexpr char kArchiveSuffix[] = "dra";
inline constexpr char kArchiveManifest[] = "archive.json";

struct ArchiveOptions
{
    bool proxyMedia = false; ///< copy the linked proxy files
    bool optimizedMedia = false; ///< copy the linked optimized media files
    QStringList renderCacheFiles; ///< render cache chunk files to copy into Cache/
};

struct ArchiveResult
{
    Status status = Status::Failed;
    QString error;
    QString projectFile; ///< the project inside the archive folder
    int mediaFiles = 0; ///< media files copied (image sequence frames counted as one file each)
    int lutFiles = 0;
    int proxyFiles = 0; ///< proxy and optimized media files
    int cacheFiles = 0;
    qint64 bytesCopied = 0;
    QStringList skipped; ///< media left linked: offline files (with reason)
};

/// True if @p folder is a Kinoklip project archive (has a readable archive.json).
bool isArchiveFolder(const QString& folder);

/// Export Project Archive: writes @p folder (created; must not exist or be empty) from the
/// project document @p projectJson, whose relative media paths are resolved against
/// @p baseDir (the folder of the project file). Media inside a single-file project are
/// copied out. On failure or cancel the folder is removed again.
ArchiveResult exportArchive(const QByteArray& projectJson, const QString& baseDir,
    const QString& projectName, const QString& folder, const ArchiveOptions& options,
    const Progress& progress = {});

struct RestoreResult
{
    Status status = Status::Failed;
    QString error;
    QString projectFile; ///< the restored project (a linked project in the new folder)
    int files = 0;
    int cacheFiles = 0;
};

/// Restore Project Archive: copies the archive @p archiveFolder to the new folder
/// @p targetFolder (must not exist or be empty) and points the project's media, LUTs and proxy
/// links at the copies. Cache chunks go to @p cacheFolder (when not empty; existing files are
/// kept). Never overwrites anything.
RestoreResult restoreArchive(const QString& archiveFolder, const QString& targetFolder,
    const QString& cacheFolder = {}, const Progress& progress = {});

} // namespace kk::pack
