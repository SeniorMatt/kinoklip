// SPDX-License-Identifier: GPL-3.0-or-later
// GradeGallery — the Color page's gallery of stills: a grabbed picture plus the grade of the
// clip it was grabbed from, saved with the project ("gallery", format version 16).
#pragma once

#include "core/Grade.h"

#include <QImage>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QVector>

namespace kk {

/// One gallery still.
struct GalleryStill
{
    quint64 id = 0; ///< unique within the gallery
    QString name; ///< "1.1", "1.2", ... (Resolve-like numbering) unless renamed
    qint64 createdMs = 0; ///< ms since the epoch
    ClipGrade grade; ///< the grade of the clip at grab time
    QImage image; ///< reduced picture (at most kGalleryImageWidth wide), ungraded alpha
};

/// Width of the stored still pictures (they are thumbnails for the gallery and the wipe-free
/// "apply grade" workflow, not reference frames).
inline constexpr int kGalleryImageWidth = 320;

/// The stills of a project, owned by Project (Project::gallery()). Not part of the undo stack
/// (like Resolve's gallery); every change marks the project modified. GUI thread only.
class GradeGallery : public QObject
{
    Q_OBJECT
public:
    explicit GradeGallery(QObject* parent = nullptr);

    [[nodiscard]] const QVector<GalleryStill>& stills() const { return m_stills; }
    /// The still with @p id, or nullptr.
    [[nodiscard]] const GalleryStill* still(quint64 id) const;
    /// Adds a still of @p image (scaled down to kGalleryImageWidth) with @p grade; returns its id.
    quint64 addStill(const QImage& image, const ClipGrade& grade);
    /// Removes still @p id (false if there is none).
    bool removeStill(quint64 id);
    /// Renames still @p id.
    bool renameStill(quint64 id, const QString& name);
    /// Removes every still.
    void clear();

    /// JSON array of the stills (images as base64 JPEG).
    [[nodiscard]] QJsonArray toJson() const;
    /// Replaces the stills with @p json (toJson() form); malformed entries are skipped.
    void fromJson(const QJsonArray& json);

Q_SIGNALS:
    /// The list or a still changed (also emitted by clear() and fromJson()).
    void changed();

private:
    QVector<GalleryStill> m_stills;
};

} // namespace kk
