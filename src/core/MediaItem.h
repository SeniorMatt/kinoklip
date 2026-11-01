// SPDX-License-Identifier: GPL-3.0-or-later
// Value types of the media pool: imported media items and the bins (folders) holding them.
#pragma once

#include "media/MediaInfo.h"

#include <QString>
#include <QVector>

namespace cw {

/// One imported media file. The probe result is cached so projects open even when the file is
/// offline (moved/deleted).
struct MediaItem
{
    QString id; ///< unique, stable across save/load (referenced by ClipData::mediaId)
    QString path; ///< absolute file path
    QString name; ///< display name (defaults to the file name)
    QString binId; ///< owning Bin::id; empty = the root ("Master") bin
    media::MediaInfo info; ///< result of media::probe() at import time

    /// Compares every field, including the cached probe result.
    friend bool operator==(const MediaItem& a, const MediaItem& b);
};

/// A folder in the media pool. Bins form a tree via parentId (empty = child of the root bin).
struct Bin
{
    QString id;
    QString name;
    QString parentId;

    friend bool operator==(const Bin&, const Bin&) = default;
};

/// Complete media pool contents; used for undo snapshots and serialization.
struct MediaPoolState
{
    QVector<Bin> bins;
    QVector<MediaItem> items;

    friend bool operator==(const MediaPoolState&, const MediaPoolState&) = default;
};

/// Field-by-field equality of two probe results (media::MediaInfo has no operator==).
[[nodiscard]] bool sameMediaInfo(const media::MediaInfo& a, const media::MediaInfo& b);

} // namespace cw
