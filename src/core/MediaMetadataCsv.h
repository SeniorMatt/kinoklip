// SPDX-License-Identifier: GPL-3.0-or-later
// MediaMetadataCsv — the clip metadata of the Media Pool as a CSV file and back (Resolve's
// File > Export > Metadata From > Media Pool / Import > Metadata To > Media Pool).
#pragma once

#include "core/MediaPool.h"

#include <QString>
#include <QStringList>

namespace kk::mediametadata {

/// Header of every exported column, in order. Import reads any of them in any order (header
/// names are compared without case); columns it does not know are ignored.
[[nodiscard]] QStringList columnNames();

/// CSV (RFC 4180, header line, CRLF line ends, UTF-8) of the media files in @p pool: one row
/// per imported file with its file name, folder, user metadata (Description, Comments,
/// Keywords, Scene, Shot, Take, Camera #, Angle, Good Take, Rating), Clip Color and Flags.
/// Timelines, compound clips and subtitle files are not listed. Keywords are separated by ", ".
[[nodiscard]] QString toCsv(const MediaPool& pool);

/// What importing a CSV file did.
struct ImportReport
{
    int rows = 0; ///< data rows read
    int updated = 0; ///< clips whose metadata changed
    int unchanged = 0; ///< clips found whose metadata already matched
    QStringList unmatched; ///< "File Name" of rows that match no clip
    QStringList ambiguous; ///< "File Name" of rows that match several clips and name no folder
    QString error; ///< empty = the file could be read; else why not (no "File Name" column ...)

    [[nodiscard]] bool ok() const { return error.isEmpty(); }
};

/// Applies @p csv to the media files of @p pool. A row finds its clip by "File Name" and, when
/// the file has a "Clip Directory" column, that folder too (a file name that is used by several
/// clips and is not told apart by a folder is reported as ambiguous and skipped). Only the
/// columns present in the file are applied, and a cell left empty keeps the clip's value, so a
/// file with a few columns never wipes the rest. Does not open an undo step; the caller does.
ImportReport applyCsv(MediaPool& pool, const QString& csv);

/// Splits CSV text into records of fields (quotes, doubled quotes, line breaks inside quotes,
/// a leading byte order mark, CRLF / LF / CR line ends). Exposed for tests.
[[nodiscard]] QList<QStringList> parseCsv(const QString& text);

} // namespace kk::mediametadata
