// SPDX-License-Identifier: GPL-3.0-or-later
// MediaStore — where imported media go: a single-file project copies every imported file into
// its project file (src/pack), a linked project uses the files where they are. Implemented by
// the app (PackageController); the import paths in src/ui reach it through AppContext.
#pragma once

#include <QString>
#include <QStringList>

class QWidget;

namespace kk {

class MediaStore
{
public:
    virtual ~MediaStore() = default;

    /// True while the open project keeps its media inside the project file.
    [[nodiscard]] virtual bool storesMedia() const = 0;
    /// The open single-file project's file (absolute), or empty for a linked project.
    [[nodiscard]] virtual QString containerPath() const = 0;
    /// Copies the files of @p paths into the project file (on a worker thread, with a
    /// progress dialog and Cancel, parented to @p parent) and replaces each by the path the
    /// media pool uses for the stored copy. Paths that cannot be stored (folders, image
    /// sequences, media already inside this project) stay as they are. A file stored before is
    /// not copied again (a notice says so). Returns false if the user cancelled or the copy
    /// failed (the user was told): nothing should be imported then. A linked project returns
    /// true and leaves @p paths unchanged.
    virtual bool storeMedia(QStringList& paths, QWidget* parent) = 0;
};

} // namespace kk
