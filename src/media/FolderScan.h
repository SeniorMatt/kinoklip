// SPDX-License-Identifier: GPL-3.0-or-later
// FolderScan — walks a folder tree for File > Import Media Folder: the importable files of each
// folder in natural order, numbered stills folded into image sequences, hidden and system files
// skipped, symlink loops cut.
#pragma once

#include "core/Rational.h"
#include "media/ImageSequence.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <functional>
#include <vector>

namespace kk::media {

/// What scanMediaFolder() looks for.
struct FolderScanOptions
{
    /// Walk into subfolders (false: only the files directly inside the folder).
    bool includeSubfolders = true;
    /// Lower-case extensions (without dot) of the files to list. Empty: every extension of
    /// SupportedFormats::allExtensions().
    QSet<QString> extensions;
    /// Fold numbered stills of one folder (shot_0001.png, shot_0002.png ...) into one image
    /// sequence path (ImageSequence.h) instead of listing each file.
    bool detectSequences = true;
    /// Frame rate written into the sequence paths.
    Rational sequenceFrameRate = ImageSequence::kDefaultFrameRate;
    /// Safety bound on the folder depth (deeper folders are not entered).
    int maxDepth = 64;
};

/// One scanned folder: its importable files and its subfolders, both in natural order.
struct ScannedFolder
{
    QString name; ///< folder name (the bin name for "create bins")
    QString path; ///< absolute path as reached (through symlinks, not canonicalised)
    /// Absolute file paths and image sequence paths, natural order of the file names (a
    /// sequence sorts at its first frame's name).
    QStringList files;
    std::vector<ScannedFolder> subfolders; ///< natural order of the names

    /// Files of this folder and every subfolder.
    [[nodiscard]] int totalFiles() const;
};

/// Outcome of scanMediaFolder().
struct FolderScanResult
{
    bool ok = false; ///< false: the folder does not exist / is unreadable, or cancelled
    bool cancelled = false; ///< the progress callback asked to stop
    ScannedFolder root;
    int skippedHidden = 0; ///< hidden files / folders and system junk (.DS_Store, Thumbs.db ...)
    int skippedUnsupported = 0; ///< files whose extension is not importable
    int skippedLinks = 0; ///< folders not entered again (symlink loops, a folder reached twice)
};

/// Called now and then with the number of directory entries looked at so far; return false
/// to stop the scan.
using FolderScanProgress = std::function<bool(int entriesSeen)>;

/// Scans @p folder (see FolderScanOptions). Symlinked files are listed; symlinked folders are
/// entered unless their target was visited before (a loop or a second link to the same folder,
/// counted in skippedLinks), so the scan always ends. Re-entrant (reads the file system only),
/// meant for a worker thread.
[[nodiscard]] FolderScanResult scanMediaFolder(const QString& folder,
    const FolderScanOptions& options = {}, const FolderScanProgress& progress = {});

/// True for names the import skips as hidden or system files: a leading '.', and the junk
/// Finder / Explorer leave behind (.DS_Store, Thumbs.db, desktop.ini; case-insensitive).
[[nodiscard]] bool isHiddenOrJunkName(QStringView name);

/// Natural ("human") order of names: runs of ASCII digits compare by value ("clip2" before
/// "clip10"), other characters case-insensitively; ties fall back to fewer leading zeros, then
/// to the case-sensitive order, so the order is total. Returns <0, 0 or >0.
[[nodiscard]] int naturalCompare(QStringView a, QStringView b);
/// naturalCompare(a, b) < 0, for std::sort.
[[nodiscard]] inline bool naturalLess(QStringView a, QStringView b)
{
    return naturalCompare(a, b) < 0;
}

} // namespace kk::media
