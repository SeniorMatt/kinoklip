// SPDX-License-Identifier: GPL-3.0-or-later
// MediaMetadataDump — everything libavformat knows about a media file as a flat list of
// group / key / value rows (the Metadata panel's "full view", like `ffprobe -show_format
// -show_streams -show_chapters`): container properties and tags (including non-standard ones
// such as the QuickTime "mdta" keys "com.dji.camera.ColorGammaSxS"), chapters, and for every
// stream its codec parameters, tags, disposition and side data (rotation matrix, HDR mastering
// display, content light level ...). Read only; nothing is cached.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace kk::media {

/// One row of the dump.
struct MetadataEntry
{
    QString group; ///< "Container", "Container tags", "Chapter 1", "Video stream #0", ...
    QString key; ///< "format_name", "com.dji.camera.ColorGammaSxS", "width", ...
    QString value;

    friend bool operator==(const MetadataEntry&, const MetadataEntry&) = default;
};

/// Reads the dump of @p path (an embedded path of a single-file project works too). An
/// unreadable file gives an empty list and a reason in @p error.
[[nodiscard]] QVector<MetadataEntry> readAllMetadata(const QString& path, QString* error = nullptr);

/// The rows whose group, key or value contains every word of @p filter (case-insensitive;
/// empty filter = all rows).
[[nodiscard]] QVector<MetadataEntry> filterMetadata(
    const QVector<MetadataEntry>& rows, const QString& filter);

/// The rows as text, one "key : value" line per row under "[group]" headings (Copy all).
[[nodiscard]] QString metadataToText(const QVector<MetadataEntry>& rows);

} // namespace kk::media
