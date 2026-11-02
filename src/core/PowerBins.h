// SPDX-License-Identifier: GPL-3.0-or-later
// PowerBins — media bins shared by every project (logos, music beds, ...), stored in the
// application data folder instead of the project file.
#pragma once

#include "core/MediaPool.h"

#include <QObject>
#include <QString>

namespace kk {

/// The power bins: an application-wide MediaPool (bins + items) saved to its own JSON file
/// (default: `<AppDataLocation>/power-bins.json`). Items reference their files by absolute
/// path. Every change is written back immediately (the file is small: bins and probe results,
/// no media), so all projects and later sessions see the same contents. Changes are not part
/// of any project's undo history.
///
/// Using a power bin item in a project (timeline drop, source viewer) adds a copy of it to the
/// project's media pool (see MediaPoolPanel::ensureInProject()).
/// Thread-safety: GUI thread only.
class PowerBins : public QObject
{
    Q_OBJECT
public:
    /// Creates the store backed by @p filePath (empty = defaultFilePath()) and loads it.
    explicit PowerBins(const QString& filePath = {}, QObject* parent = nullptr);

    /// `<AppDataLocation>/power-bins.json`.
    [[nodiscard]] static QString defaultFilePath();

    /// The shared bins and items (never null). Changes made to it are saved automatically.
    [[nodiscard]] MediaPool* pool() const { return m_pool; }
    /// The JSON file.
    [[nodiscard]] QString filePath() const { return m_filePath; }

    /// Re-reads the file (missing file = empty). False with @p error for an unreadable file;
    /// the contents are then empty and a copy of the file is kept as `<file>.bak`.
    bool load(QString* error = nullptr);
    /// Writes the file now. False with @p error on failure.
    bool save(QString* error = nullptr) const;

private:
    MediaPool* m_pool = nullptr;
    QString m_filePath;
    bool m_loading = false;
};

} // namespace kk
