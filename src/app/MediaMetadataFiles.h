// SPDX-License-Identifier: GPL-3.0-or-later
// MediaMetadataFiles — File > Import > Metadata To Media Pool and File > Export > Metadata From
// Media Pool: the clip metadata of the Media Pool as a CSV file and back (spec 166).
#pragma once

#include <QString>

class QWidget;

namespace kk {

struct AppContext;

/// Connects `file.import_metadata` and `file.export_metadata` of @p ctx's registry. Export asks
/// for a file name (the project's name suggested) and writes core/MediaMetadataCsv.h's CSV;
/// import asks for a CSV file, applies it in one undo step "Import Metadata" and tells what
/// changed and which rows matched no clip. Errors show in a message box parented to
/// @p window. Call once per main window.
void connectMediaMetadataActions(AppContext& ctx, QWidget* window);

} // namespace kk
