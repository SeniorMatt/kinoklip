// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineExport — File > Export: the active timeline as an EDL or an OpenTimelineIO file, its
// markers as an EDL or CSV (spec 18.12, 18.13, 51.1).
#pragma once

class QWidget;

namespace kk {

struct AppContext;

/// Connects `file.export_edl`, `file.export_otio`, `file.export_markers_edl` and `file.export_markers_csv` of
/// @p ctx's registry: each asks for a file name (the timeline's name suggested) and writes the
/// active timeline with export/EdlExport.h; a write error is shown in a message box parented
/// to @p window. Call once per main window.
void connectTimelineExportActions(AppContext& ctx, QWidget* window);

} // namespace kk
