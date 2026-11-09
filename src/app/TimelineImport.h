// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineImport — File > Import > Timeline: an OpenTimelineIO (.otio) file becomes a new
// timeline of the project (spec 51.1 - 51.4).
#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

class QWidget;

namespace kk {

class Project;
struct AppContext;

/// Outcome of importOtioFile().
struct TimelineImportReport
{
    quint64 uid = 0; ///< the new timeline; 0 = nothing was imported (see error)
    QString error;
    int clipCount = 0;
    int mediaAdded = 0; ///< new media pool items for files that exist
    QStringList offline; ///< files that do not exist: their clips are offline
    QStringList skipped; ///< what the file held that Kinoklip left out (otio::TimelineImport)
};

/// Reads the .otio file @p path into a new timeline of @p project (bin @p binId, "" = Master) as
/// one undo step "Import Timeline": referenced files that exist go through the media pool's
/// import (a file already in the pool is reused), missing ones become offline pool items next
/// to the .otio file (relinking works as for any offline media). The new timeline is not
/// activated. Blocking; GUI thread.
TimelineImportReport importOtioFile(
    Project& project, const QString& path, const QString& binId = {});

/// Connects `file.import_timeline` of @p ctx's registry: asks for a .otio file, imports it into
/// the bin the media pool shows, opens the new timeline and tells what was left out in a message
/// box parented to @p window. Call once per main window.
void connectTimelineImportActions(AppContext& ctx, QWidget* window);

} // namespace kk
