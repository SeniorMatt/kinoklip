// SPDX-License-Identifier: GPL-3.0-or-later
// SaveMediaCopy — Media Pool > "Save a Copy to Computer…": writes the original file of media
// stored inside a single-file project back out to the computer (byte for byte, the project
// stays unchanged). The pure parts (which items qualify, default folder, file names) are free
// functions so they can be tested without dialogs.
#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QWidget;

namespace kk {

struct AppContext;
class MediaPool;
struct MediaItem;

/// One stored media file that can be copied out.
struct StoredCopySource
{
    QString mediaId;
    QString container; ///< the single-file project
    QString entry; ///< entry name inside it
    QString fileName; ///< original file name with extension
};

/// The items of @p ids whose media is stored in a project file (linked files, missing files,
/// subtitles without a file and timelines are left out). Order of @p ids.
[[nodiscard]] QVector<StoredCopySource> storedCopySources(
    const MediaPool& pool, const QStringList& ids);

/// First existing folder of: the original folder (when recorded), the standard folder for the
/// media type (@p typeFolder), the project's folder; the home folder as the last resort.
[[nodiscard]] QString defaultCopyFolder(
    const QString& originalFolder, const QString& typeFolder, const QString& projectFolder);
/// The standard folder for the kind of media of @p item: Music (audio only), Pictures (still
/// images), Videos otherwise.
[[nodiscard]] QString typeFolderFor(const MediaItem& item);

/// @p fileName, or "name (2).ext", "name (3).ext"... when a file of that name exists in
/// @p folder or is in @p taken (case-sensitive names of files chosen earlier in the same run).
[[nodiscard]] QString uniqueCopyName(
    const QString& folder, const QString& fileName, const QSet<QString>& taken = {});

/// Copies the stored media of @p ids out: one item asks for a file name (save dialog), several
/// for a folder (clashing names get " (2)"). Worker thread with progress and Cancel; a
/// cancelled copy leaves no partial file (finished files stay). Returns the number of files
/// written. Failures are shown to the user.
int saveCopiesToComputer(AppContext& ctx, const QStringList& ids, QWidget* parent);

} // namespace kk
