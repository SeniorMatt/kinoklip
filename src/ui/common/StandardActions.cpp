// SPDX-License-Identifier: GPL-3.0-or-later
// Table of every standard command. Default shortcuts live in
// resources/keymaps/resolve-windows.json.
#include "ui/common/StandardActions.h"

#include "ui/common/ActionRegistry.h"

#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>

namespace kk {

namespace {

const char* const kContext = "kk::StandardActions";

/// One row of the catalogue. id "-" = separator in the current category; an id starting with
/// ">" (ActionRegistry::kSubmenuPrefix) is a submenu titled @c text showing that category,
/// whose actions are registered by their owner (e.g. src/app/Theme for the colour schemes).
struct ActionSpec
{
    const char* category;
    const char* id;
    const char* text = nullptr;
    const char* icon = "";
    bool checkable = false;
};

// clang-format off
const ActionSpec kActions[] = {
    // ---- File ----
    {"file", "file.project_manager",  QT_TRANSLATE_NOOP("kk::StandardActions", "Project &Manager…")},
    {"file", "-"},
    {"file", "file.new",              QT_TRANSLATE_NOOP("kk::StandardActions", "&New Project")},
    {"file", "timeline.new",          QT_TRANSLATE_NOOP("kk::StandardActions", "New &Timeline…"), "timeline"},
    {"file", "mediapool.new_bin",     QT_TRANSLATE_NOOP("kk::StandardActions", "New &Bin"), "folder"},
    {"file", "mediapool.new_smart_bin", QT_TRANSLATE_NOOP("kk::StandardActions", "New S&mart Bin…"), "smart_bin"},
    {"file", "file.open",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Open Project…")},
    {"file", ">file.recent",          QT_TRANSLATE_NOOP("kk::StandardActions", "Open &Recent")},
    {"file", "-"},
    {"file", "file.save",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Save Project")},
    {"file", "file.save_as",          QT_TRANSLATE_NOOP("kk::StandardActions", "Save Project &As…")},
    // Single-file projects (src/app/ProjectPackaging.cpp): convert between the kinds, clean up.
    {"file", "file.pack_project",     QT_TRANSLATE_NOOP("kk::StandardActions", "Pac&k Project…")},
    {"file", "file.unpack_project",   QT_TRANSLATE_NOOP("kk::StandardActions", "U&npack Project…")},
    {"file", "file.compact_project",  QT_TRANSLATE_NOOP("kk::StandardActions", "Co&mpact Project…")},
    {"file", "file.verify_project",   QT_TRANSLATE_NOOP("kk::StandardActions", "&Verify Project File")},
    {"file", "-"},
    {"file", "file.import_media",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Import Media…"), "import"},
    {"file", "file.import_media_folder", QT_TRANSLATE_NOOP("kk::StandardActions", "Import Media &Folder…"), "folder"},
    {"file", "file.import_subtitle",  QT_TRANSLATE_NOOP("kk::StandardActions", "Import S&ubtitle…")},
    // The media pool's proxy / optimized media commands also live here (act on the media
    // pool selection); Resolve offers them in the media pool context menu.
    // File > Export: the timeline as an edit decision list, its markers as EDL / CSV.
    {"file", ">file.export",          QT_TRANSLATE_NOOP("kk::StandardActions", "E&xport")},
    {"file.export", "file.export_edl", QT_TRANSLATE_NOOP("kk::StandardActions", "Timeline as &EDL…")},
    {"file.export", "file.export_markers_edl", QT_TRANSLATE_NOOP("kk::StandardActions", "Timeline &Markers to EDL…")},
    {"file.export", "file.export_markers_csv", QT_TRANSLATE_NOOP("kk::StandardActions", "Timeline Markers to &CSV…")},
    {"file", ">mediapool",            QT_TRANSLATE_NOOP("kk::StandardActions", "Pro&xy and Optimized Media")},
    // Media pool organisation commands acting on the visible media pool (bins branch).
    {"file", ">mediapool.organize",   QT_TRANSLATE_NOOP("kk::StandardActions", "Media &Pool")},
    {"file", "-"},
    {"file", "file.project_settings", QT_TRANSLATE_NOOP("kk::StandardActions", "Project Se&ttings…")},
    {"file", "-"},
    {"file", "file.quit",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Quit")},

    // ---- Edit ----
    {"edit", "edit.undo",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Undo")},
    {"edit", "edit.redo",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Redo")},
    {"edit", ">edit.history",         QT_TRANSLATE_NOOP("kk::StandardActions", "&History")},
    {"edit.history", "edit.history_window", QT_TRANSLATE_NOOP("kk::StandardActions", "Open History &Window…")},
    {"edit", "-"},
    {"edit", "edit.cut",              QT_TRANSLATE_NOOP("kk::StandardActions", "Cu&t")},
    {"edit", "edit.copy",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Copy")},
    {"edit", "edit.paste",            QT_TRANSLATE_NOOP("kk::StandardActions", "&Paste")},
    {"edit", "edit.paste_insert",     QT_TRANSLATE_NOOP("kk::StandardActions", "Paste &Insert")},
    {"edit", "edit.paste_attributes", QT_TRANSLATE_NOOP("kk::StandardActions", "Paste Attri&butes…")},
    {"edit", "edit.ripple_cut",       QT_TRANSLATE_NOOP("kk::StandardActions", "Ripple Cu&t")},
    {"edit", "-"},
    {"edit", "edit.delete",           QT_TRANSLATE_NOOP("kk::StandardActions", "&Delete Selected")},
    {"edit", "edit.ripple_delete",    QT_TRANSLATE_NOOP("kk::StandardActions", "Ripple De&lete")},
    {"edit", "edit.delete_gaps",      QT_TRANSLATE_NOOP("kk::StandardActions", "Delete &Gaps")},
    {"edit", "-"},
    {"edit", "edit.select_all",       QT_TRANSLATE_NOOP("kk::StandardActions", "Select &All")},
    {"edit", "edit.deselect_all",     QT_TRANSLATE_NOOP("kk::StandardActions", "Deselect All")},
    {"edit", "edit.select_forward",   QT_TRANSLATE_NOOP("kk::StandardActions", "Select Clips Forward")},
    {"edit", "edit.select_backward",  QT_TRANSLATE_NOOP("kk::StandardActions", "Select Clips Backward")},
    {"edit", "edit.select_forward_all", QT_TRANSLATE_NOOP("kk::StandardActions", "Select Clips Forward on All Tracks")},
    {"edit", "edit.select_backward_all", QT_TRANSLATE_NOOP("kk::StandardActions", "Select Clips Backward on All Tracks")},
    {"edit", "-"},
    {"edit", "edit.insert",           QT_TRANSLATE_NOOP("kk::StandardActions", "&Insert Clip"), "insert"},
    {"edit", "edit.overwrite",        QT_TRANSLATE_NOOP("kk::StandardActions", "&Overwrite Clip"), "overwrite"},
    {"edit", "edit.ripple_overwrite", QT_TRANSLATE_NOOP("kk::StandardActions", "Ripple O&verwrite")},
    {"edit", "edit.place_on_top",     QT_TRANSLATE_NOOP("kk::StandardActions", "Place on &Top"), "place_on_top"},
    {"edit", "edit.replace",          QT_TRANSLATE_NOOP("kk::StandardActions", "&Replace Clip"), "replace"},
    {"edit", "edit.fit_to_fill",      QT_TRANSLATE_NOOP("kk::StandardActions", "&Fit to Fill")},
    {"edit", "edit.append_at_end",    QT_TRANSLATE_NOOP("kk::StandardActions", "Append at &End")},
    // Cut page smart edits and viewer modes (enabled while the Cut page is shown).
    {"edit", ">cut",                  QT_TRANSLATE_NOOP("kk::StandardActions", "C&ut Page")},
    // Node editor commands of the Compositing page (keys act while the node editor has focus).
    {"edit", ">comp",                 QT_TRANSLATE_NOOP("kk::StandardActions", "Co&mpositing Page")},
    // Grade commands of the Color page (enabled while it is shown; keys act on the page).
    {"edit", ">color",                QT_TRANSLATE_NOOP("kk::StandardActions", "Co&lor Page")},
    {"edit", "-"},
    {"edit", "edit.keyboard_customization", QT_TRANSLATE_NOOP("kk::StandardActions", "&Keyboard Customization…")},
    {"edit", "edit.preferences",      QT_TRANSLATE_NOOP("kk::StandardActions", "Pr&eferences…")},

    // ---- Trim (edit tools; the three tools form an exclusive group) ----
    {"trim", "tool.select",           QT_TRANSLATE_NOOP("kk::StandardActions", "&Selection Mode"), "select", true},
    {"trim", "tool.trim",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Trim Edit Mode"), "trim", true},
    {"trim", "tool.blade",            QT_TRANSLATE_NOOP("kk::StandardActions", "&Blade Edit Mode"), "blade", true},
    {"trim", "-"},
    {"trim", "trim.dynamic",          QT_TRANSLATE_NOOP("kk::StandardActions", "&Dynamic Trim Mode"), "", true},
    {"trim", "-"},
    {"trim", "trim.nudge_left",       QT_TRANSLATE_NOOP("kk::StandardActions", "Nudge One Frame &Left")},
    {"trim", "trim.nudge_right",      QT_TRANSLATE_NOOP("kk::StandardActions", "Nudge One Frame &Right")},
    {"trim", "trim.nudge_left_multi", QT_TRANSLATE_NOOP("kk::StandardActions", "Nudge Several Frames Left")},
    {"trim", "trim.nudge_right_multi", QT_TRANSLATE_NOOP("kk::StandardActions", "Nudge Several Frames Right")},
    {"trim", "-"},
    {"trim", "trim.trim_start",       QT_TRANSLATE_NOOP("kk::StandardActions", "Trim &Start to Playhead")},
    {"trim", "trim.trim_end",         QT_TRANSLATE_NOOP("kk::StandardActions", "Trim &End to Playhead")},
    {"trim", "trim.ripple_trim_start", QT_TRANSLATE_NOOP("kk::StandardActions", "Ripple Trim Start to Playhead")},
    {"trim", "trim.ripple_trim_end",  QT_TRANSLATE_NOOP("kk::StandardActions", "Ripple Trim End to Playhead")},
    {"trim", "trim.extend_edit",      QT_TRANSLATE_NOOP("kk::StandardActions", "E&xtend Edit to Playhead")},
    {"trim", "trim.cycle_edit",       QT_TRANSLATE_NOOP("kk::StandardActions", "Cycle Edit Point &Side")},
    {"trim", "-"},
    {"trim", "trim.select_nearest_edit", QT_TRANSLATE_NOOP("kk::StandardActions", "Select &Nearest Edit Point")},
    {"trim", "trim.select_nearest_clip", QT_TRANSLATE_NOOP("kk::StandardActions", "Select Nearest &Clip or Gap")},
    {"trim", "-"},
    {"trim", "trim.swap_left",        QT_TRANSLATE_NOOP("kk::StandardActions", "Swap Clip with Previous")},
    {"trim", "trim.swap_right",       QT_TRANSLATE_NOOP("kk::StandardActions", "Swap Clip with Next")},
    {"trim", "trim.move_up",          QT_TRANSLATE_NOOP("kk::StandardActions", "Move Clips Up a Track")},
    {"trim", "trim.move_down",        QT_TRANSLATE_NOOP("kk::StandardActions", "Move Clips Down a Track")},

    // ---- Timeline ----
    {"timeline", "edit.split",        QT_TRANSLATE_NOOP("kk::StandardActions", "&Split Clip at Playhead"), "split"},
    {"timeline", "edit.join",         QT_TRANSLATE_NOOP("kk::StandardActions", "&Join Clips")},
    {"timeline", "timeline.add_transition", QT_TRANSLATE_NOOP("kk::StandardActions", "Add Default &Transition")},
    {"timeline", "-"},
    {"timeline", "timeline.snapping", QT_TRANSLATE_NOOP("kk::StandardActions", "S&napping"), "snap", true},
    {"timeline", "timeline.linked_selection", QT_TRANSLATE_NOOP("kk::StandardActions", "&Linked Selection"), "link", true},
    {"timeline", "timeline.audio_scrubbing", QT_TRANSLATE_NOOP("kk::StandardActions", "Audio Scr&ubbing"), "", true},
    {"timeline", "-"},
    {"timeline", "timeline.zoom_in",  QT_TRANSLATE_NOOP("kk::StandardActions", "Zoom &In"), "zoom_in"},
    {"timeline", "timeline.zoom_out", QT_TRANSLATE_NOOP("kk::StandardActions", "Zoom &Out"), "zoom_out"},
    {"timeline", "timeline.zoom_fit", QT_TRANSLATE_NOOP("kk::StandardActions", "Zoom to &Fit"), "zoom_fit"},
    {"timeline", "-"},
    {"timeline", "timeline.add_video_track", QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Video Track")},
    {"timeline", "timeline.add_audio_track", QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Audio Track")},
    {"timeline", "timeline.add_subtitle_track", QT_TRANSLATE_NOOP("kk::StandardActions", "Add Su&btitle Track")},
    {"timeline", "timeline.add_subtitle", QT_TRANSLATE_NOOP("kk::StandardActions", "Add Subtit&le")},
    {"timeline", "timeline.delete_empty_tracks", QT_TRANSLATE_NOOP("kk::StandardActions", "Delete &Empty Tracks")},
    {"timeline", "-"},
    {"timeline", "timeline.duplicate", QT_TRANSLATE_NOOP("kk::StandardActions", "D&uplicate Timeline")},
    {"timeline", "timeline.delete",   QT_TRANSLATE_NOOP("kk::StandardActions", "Delete Timel&ine")},
    {"timeline", "timeline.settings", QT_TRANSLATE_NOOP("kk::StandardActions", "Timeline Se&ttings…")},
    {"timeline", "timeline.next_tab", QT_TRANSLATE_NOOP("kk::StandardActions", "Ne&xt Timeline Tab")},
    {"timeline", "timeline.prev_tab", QT_TRANSLATE_NOOP("kk::StandardActions", "Previous Timeline Ta&b")},
    {"timeline", "timeline.close_tab", QT_TRANSLATE_NOOP("kk::StandardActions", "&Close Timeline Tab")},
    {"timeline", "-"},
    {"timeline", ">timeline.tracks",  QT_TRANSLATE_NOOP("kk::StandardActions", "Track &Destination and Auto Select")},

    // ---- Clip ----
    {"clip", "clip.toggle_enabled",   QT_TRANSLATE_NOOP("kk::StandardActions", "&Enable / Disable Clip")},
    {"clip", "clip.flag",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Flag Clip"), "flag"},
    {"clip", "clip.match_frame",      QT_TRANSLATE_NOOP("kk::StandardActions", "&Match Frame")},
    {"clip", "timeline.find_in_media_pool", QT_TRANSLATE_NOOP("kk::StandardActions", "&Find in Media Pool")},
    {"clip", "-"},
    {"clip", "clip.change_duration",  QT_TRANSLATE_NOOP("kk::StandardActions", "Change Clip &Duration…")},
    {"clip", "clip.change_speed",     QT_TRANSLATE_NOOP("kk::StandardActions", "Change Clip &Speed…")},
    {"clip", "clip.retime_controls",  QT_TRANSLATE_NOOP("kk::StandardActions", "&Retime Controls")},
    {"clip", "clip.speed_increase",   QT_TRANSLATE_NOOP("kk::StandardActions", "Increase Clip Speed by 10%")},
    {"clip", "clip.speed_decrease",   QT_TRANSLATE_NOOP("kk::StandardActions", "Decrease Clip Speed by 10%")},
    {"clip", "clip.freeze_frame",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Freeze Frame")},
    {"clip", "clip.retime_curve",     QT_TRANSLATE_NOOP("kk::StandardActions", "Retime &Curve")},
    {"clip", "-"},
    {"clip", "clip.new_compound",     QT_TRANSLATE_NOOP("kk::StandardActions", "New &Compound Clip…"), "compound_clip"},
    {"clip", "clip.open_in_timeline", QT_TRANSLATE_NOOP("kk::StandardActions", "&Open in Timeline")},
    {"clip", "clip.decompose",        QT_TRANSLATE_NOOP("kk::StandardActions", "Decompose in &Place")},
    {"clip", "clip.attributes",       QT_TRANSLATE_NOOP("kk::StandardActions", "Clip &Attributes…")},
    // Compositions (src/app/pages/CompositingPage): open the clip's node graph, remove it.
    {"clip", "clip.open_in_compositing", QT_TRANSLATE_NOOP("kk::StandardActions", "Open in Com&positing Page"), "page_compositing"},
    {"clip", "clip.reset_composition", QT_TRANSLATE_NOOP("kk::StandardActions", "Reset Compos&ition")},
    {"clip", "clip.normalize_audio",  QT_TRANSLATE_NOOP("kk::StandardActions", "&Normalize Audio Levels…")},
    {"clip", "clip.render_cache_output", QT_TRANSLATE_NOOP("kk::StandardActions", "Render Cache Clip O&utput"), "", true},
    {"clip", "-"},
    // Multicam clips (media pool commands in src/ui/mediapool/MediaPoolMulticam.cpp, angle
    // edits in src/ui/timeline/TimelineMulticam.cpp, viewer grid in src/ui/viewer).
    {"clip", "mediapool.new_multicam", QT_TRANSLATE_NOOP("kk::StandardActions", "New Multica&m Clip Using Selected Clips…"), "multicam_clip"},
    {"clip", ">clip.sync",            QT_TRANSLATE_NOOP("kk::StandardActions", "S&ync and Place on Timeline")},
    {"clip.sync", "mediapool.sync_waveform", QT_TRANSLATE_NOOP("kk::StandardActions", "Based on &Waveform")},
    {"clip.sync", "mediapool.sync_timecode", QT_TRANSLATE_NOOP("kk::StandardActions", "Based on &Timecode")},
    {"clip.sync", "mediapool.sync_in_points", QT_TRANSLATE_NOOP("kk::StandardActions", "Based on &In Points")},
    {"clip", ">clip.multicam",        QT_TRANSLATE_NOOP("kk::StandardActions", "Multica&m")},
    {"clip.multicam", "multicam.flatten", QT_TRANSLATE_NOOP("kk::StandardActions", "&Flatten Multicam Clip")},
    {"clip.multicam", "-"},
    {"clip.multicam", "multicam.mode_cut", QT_TRANSLATE_NOOP("kk::StandardActions", "&Cut Mode (angle clicks cut at the playhead)"), "", true},
    {"clip.multicam", "multicam.mode_switch", QT_TRANSLATE_NOOP("kk::StandardActions", "&Switch Mode (angle clicks change the current segment)"), "", true},
    {"clip.multicam", "-"},
    {"clip.multicam", "multicam.target_both", QT_TRANSLATE_NOOP("kk::StandardActions", "Video &and Audio"), "", true},
    {"clip.multicam", "multicam.target_video", QT_TRANSLATE_NOOP("kk::StandardActions", "&Video Only"), "", true},
    {"clip.multicam", "multicam.target_audio", QT_TRANSLATE_NOOP("kk::StandardActions", "A&udio Only"), "", true},
    {"clip.multicam", "-"},
    {"clip.multicam", "multicam.prev_page", QT_TRANSLATE_NOOP("kk::StandardActions", "&Previous Angle Page")},
    {"clip.multicam", "multicam.next_page", QT_TRANSLATE_NOOP("kk::StandardActions", "&Next Angle Page")},
    {"clip.multicam", "-"},
    {"clip", "-"},
    {"clip", "clip.link",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Link Clips")},
    {"clip", "clip.unlink",           QT_TRANSLATE_NOOP("kk::StandardActions", "&Unlink Clips")},
    {"clip", "-"},
    {"clip", "clip.add_keyframe",     QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Keyframe"), "keyframe_on"},
    {"clip", "clip.prev_keyframe",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Previous Keyframe"), "keyframe_prev"},
    {"clip", "clip.next_keyframe",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Next Keyframe"), "keyframe_next"},
    {"clip", "clip.delete_keyframe",  QT_TRANSLATE_NOOP("kk::StandardActions", "&Delete Keyframes")},
    {"clip", "clip.copy_keyframes",   QT_TRANSLATE_NOOP("kk::StandardActions", "&Copy Keyframes")},
    {"clip", "clip.paste_keyframes",  QT_TRANSLATE_NOOP("kk::StandardActions", "Paste Key&frames")},
    {"clip", "clip.toggle_keyframe_editor", QT_TRANSLATE_NOOP("kk::StandardActions", "Show Keyframe &Editor"), "keyframe_editor"},
    {"clip", "clip.toggle_curve_editor", QT_TRANSLATE_NOOP("kk::StandardActions", "Show C&urve Editor"), "curve_editor"},
    {"clip", "-"},
    // Masks of the current clip (src/ui/inspector/InspectorMasks.cpp; drawn in the viewer).
    {"clip", ">clip.masks",           QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Mask")},
    {"clip.masks", "mask.add_rectangle", QT_TRANSLATE_NOOP("kk::StandardActions", "&Rectangle Mask")},
    {"clip.masks", "mask.add_ellipse",   QT_TRANSLATE_NOOP("kk::StandardActions", "&Ellipse Mask")},
    {"clip.masks", "mask.add_polygon",   QT_TRANSLATE_NOOP("kk::StandardActions", "&Polygon Mask")},
    {"clip.masks", "mask.add_bezier",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Bezier Mask")},
    {"clip.masks", "mask.add_freehand",  QT_TRANSLATE_NOOP("kk::StandardActions", "&Freehand Mask")},

    // ---- Mark ----
    {"mark", "mark.in",               QT_TRANSLATE_NOOP("kk::StandardActions", "Mark &In"), "mark_in"},
    {"mark", "mark.out",              QT_TRANSLATE_NOOP("kk::StandardActions", "Mark &Out"), "mark_out"},
    {"mark", "mark.clip",             QT_TRANSLATE_NOOP("kk::StandardActions", "Mark &Clip")},
    {"mark", "mark.selection",        QT_TRANSLATE_NOOP("kk::StandardActions", "Mark &Selection")},
    {"mark", "-"},
    {"mark", "mark.clear_in",         QT_TRANSLATE_NOOP("kk::StandardActions", "Clear In")},
    {"mark", "mark.clear_out",        QT_TRANSLATE_NOOP("kk::StandardActions", "Clear Out")},
    {"mark", "mark.clear_both",       QT_TRANSLATE_NOOP("kk::StandardActions", "Clear In and Out")},
    {"mark", "-"},
    {"mark", "mark.go_to_in",         QT_TRANSLATE_NOOP("kk::StandardActions", "Go to In")},
    {"mark", "mark.go_to_out",        QT_TRANSLATE_NOOP("kk::StandardActions", "Go to Out")},
    {"mark", "-"},
    {"mark", "mark.add_marker",       QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Marker"), "marker"},
    {"mark", "mark.modify_marker",    QT_TRANSLATE_NOOP("kk::StandardActions", "Add and M&odify Marker…")},
    {"mark", "mark.delete_marker",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Delete Marker")},

    // ---- View ----
    {"view", "view.page_media",       QT_TRANSLATE_NOOP("kk::StandardActions", "&Media Page"), "page_media", true},
    {"view", "view.page_cut",         QT_TRANSLATE_NOOP("kk::StandardActions", "C&ut Page"), "page_cut", true},
    {"view", "view.page_edit",        QT_TRANSLATE_NOOP("kk::StandardActions", "&Edit Page"), "page_edit", true},
    {"view", "view.page_compositing", QT_TRANSLATE_NOOP("kk::StandardActions", "C&ompositing Page"), "page_compositing", true},
    {"view", "view.page_color",       QT_TRANSLATE_NOOP("kk::StandardActions", "&Color Page"), "page_color", true},
    {"view", "view.page_audio",       QT_TRANSLATE_NOOP("kk::StandardActions", "&Audio Page"), "page_audio", true},
    {"view", "view.page_deliver",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Deliver Page"), "page_deliver", true},
    {"view", "-"},
    {"view", "view.toggle_media_pool", QT_TRANSLATE_NOOP("kk::StandardActions", "Media Pool"), "panel_media_pool", true},
    {"view", "view.toggle_effects",   QT_TRANSLATE_NOOP("kk::StandardActions", "Effects"), "panel_effects", true},
    {"view", "view.toggle_edit_index", QT_TRANSLATE_NOOP("kk::StandardActions", "Edit Index"), "panel_edit_index", true},
    {"view", "view.toggle_keyframes", QT_TRANSLATE_NOOP("kk::StandardActions", "Keyframes"), "panel_keyframes", true},
    {"view", "view.toggle_nodes",     QT_TRANSLATE_NOOP("kk::StandardActions", "Nodes"), "panel_nodes", true},
    {"view", "view.toggle_gallery",   QT_TRANSLATE_NOOP("kk::StandardActions", "Gallery"), "image", true},
    {"view", "view.toggle_clips",     QT_TRANSLATE_NOOP("kk::StandardActions", "Clips"), "strip_view", true},
    {"view", "view.toggle_color_timeline", QT_TRANSLATE_NOOP("kk::StandardActions", "Timeline"), "timeline", true},
    {"view", "view.toggle_scopes",    QT_TRANSLATE_NOOP("kk::StandardActions", "Scopes"), "grid_view", true},
    {"view", "view.stacked_timelines", QT_TRANSLATE_NOOP("kk::StandardActions", "S&tacked Timelines"), "", true},
    {"view", "view.toggle_mixer",     QT_TRANSLATE_NOOP("kk::StandardActions", "Mixer"), "panel_mixer", true},
    {"view", "view.toggle_metadata",  QT_TRANSLATE_NOOP("kk::StandardActions", "Metadata"), "panel_metadata", true},
    {"view", "view.toggle_inspector", QT_TRANSLATE_NOOP("kk::StandardActions", "Inspector"), "panel_inspector", true},
    {"view", "view.show_bin_list",    QT_TRANSLATE_NOOP("kk::StandardActions", "Show &Bin List"), "folder", true},
    {"view", "view.video_scopes",     QT_TRANSLATE_NOOP("kk::StandardActions", "Video &Scopes"), "", true},
    {"view", "-"},
    {"view", "view.single_viewer",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Single Viewer Mode"), "single_viewer", true},
    {"view", "view.toggle_source_timeline", QT_TRANSLATE_NOOP("kk::StandardActions", "Toggle Source / Timeline Viewer")},
    {"view", "view.multicam_viewer",  QT_TRANSLATE_NOOP("kk::StandardActions", "&Multicam Viewer (source viewer shows the angles)"), "multicam_clip", true},
    {"view", "view.fullscreen_viewer", QT_TRANSLATE_NOOP("kk::StandardActions", "Full-Screen &Viewer"), "fullscreen"},
    {"view", "view.viewer_zoom_fit",  QT_TRANSLATE_NOOP("kk::StandardActions", "Viewer &Zoom to Fit"), "zoom_fit"},
    {"view", "view.onscreen_transform", QT_TRANSLATE_NOOP("kk::StandardActions", "On-Screen &Transform Controls"), "", true},
    {"view", "view.onscreen_masks",   QT_TRANSLATE_NOOP("kk::StandardActions", "On-Screen &Mask Controls"), "", true},
    {"view", "view.onscreen_crop",    QT_TRANSLATE_NOOP("kk::StandardActions", "On-Screen &Crop Controls"), "", true},
    {"view", "view.fullscreen",       QT_TRANSLATE_NOOP("kk::StandardActions", "&Full Screen"), "", true},
    // Viewer guides, rulers, safe areas and grid (spec 41; ui/viewer/ViewerOverlaySettings).
    {"view", "-"},
    {"view", "view.show_rulers", QT_TRANSLATE_NOOP("kk::StandardActions", "Show &Rulers"), "", true},
    {"view", ">view.guides", QT_TRANSLATE_NOOP("kk::StandardActions", "&Guides")},
    {"view", ">view.safe_area", QT_TRANSLATE_NOOP("kk::StandardActions", "Safe &Area")},
    {"view", "view.grid_show", QT_TRANSLATE_NOOP("kk::StandardActions", "Show G&rid"), "", true},
    {"view", ">view.grid", QT_TRANSLATE_NOOP("kk::StandardActions", "Grid T&ype")},
    {"view.guides", "view.guides_show", QT_TRANSLATE_NOOP("kk::StandardActions", "Show &Guides"), "", true},
    {"view.guides", "view.guides_lock", QT_TRANSLATE_NOOP("kk::StandardActions", "&Lock Guides"), "", true},
    {"view.guides", "view.guides_snap", QT_TRANSLATE_NOOP("kk::StandardActions", "&Snap to Guides"), "", true},
    {"view.guides", "-"},
    {"view.guides", "view.guide_add_horizontal", QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Horizontal Guide")},
    {"view.guides", "view.guide_add_vertical", QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Vertical Guide")},
    {"view.guides", "view.guides_clear", QT_TRANSLATE_NOOP("kk::StandardActions", "&Clear Guides")},
    {"view.guides", "-"},
    {"view.guides", "view.guide_color", QT_TRANSLATE_NOOP("kk::StandardActions", "Guide C&olor…")},
    {"view.guides", "-"},
    {"view.guides", ">view.guide_presets", QT_TRANSLATE_NOOP("kk::StandardActions", "Load Guide &Preset")},
    {"view.guides", "view.guide_save_preset", QT_TRANSLATE_NOOP("kk::StandardActions", "Save Guides as Pr&eset…")},
    {"view.guides", ">view.guide_presets_delete", QT_TRANSLATE_NOOP("kk::StandardActions", "&Delete Guide Preset")},
    {"view.safe_area", "view.safe_action", QT_TRANSLATE_NOOP("kk::StandardActions", "&Action Safe (90 %)"), "", true},
    {"view.safe_area", "view.safe_title", QT_TRANSLATE_NOOP("kk::StandardActions", "&Title Safe (80 %)"), "", true},
    {"view.safe_area", "view.safe_centre", QT_TRANSLATE_NOOP("kk::StandardActions", "&Centre Mark"), "", true},
    {"view.safe_area", "-"},
    {"view.safe_area", "view.aspect_none", QT_TRANSLATE_NOOP("kk::StandardActions", "&No Aspect Marker"), "", true},
    {"view.safe_area", "view.aspect_2_39", QT_TRANSLATE_NOOP("kk::StandardActions", "2.39:1 (&Widescreen)"), "", true},
    {"view.safe_area", "view.aspect_1_85", QT_TRANSLATE_NOOP("kk::StandardActions", "1.85:1 (&Flat)"), "", true},
    {"view.safe_area", "view.aspect_4_3", QT_TRANSLATE_NOOP("kk::StandardActions", "&4:3"), "", true},
    {"view.safe_area", "view.aspect_1_1", QT_TRANSLATE_NOOP("kk::StandardActions", "1:1 (&Square)"), "", true},
    {"view.safe_area", "view.aspect_4_5", QT_TRANSLATE_NOOP("kk::StandardActions", "4:5 (&Portrait)"), "", true},
    {"view.safe_area", "view.aspect_9_16", QT_TRANSLATE_NOOP("kk::StandardActions", "9:16 (&Vertical Video)"), "", true},
    {"view.grid", "view.grid_thirds", QT_TRANSLATE_NOOP("kk::StandardActions", "&Rule of Thirds"), "", true},
    {"view.grid", "view.grid_quarters", QT_TRANSLATE_NOOP("kk::StandardActions", "&4 x 4 Grid"), "", true},
    {"view.grid", "view.grid_custom", QT_TRANSLATE_NOOP("kk::StandardActions", "&Custom Grid…"), "", true},

    // ---- Playback ----
    {"playback", "transport.play_pause",      QT_TRANSLATE_NOOP("kk::StandardActions", "&Play / Pause"), "play"},
    {"playback", "transport.play_reverse",    QT_TRANSLATE_NOOP("kk::StandardActions", "Play Re&verse")},
    {"playback", "transport.shuttle_reverse", QT_TRANSLATE_NOOP("kk::StandardActions", "Shuttle &Reverse")},
    {"playback", "transport.shuttle_stop",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Stop"), "stop"},
    {"playback", "transport.shuttle_forward", QT_TRANSLATE_NOOP("kk::StandardActions", "Shuttle &Forward")},
    {"playback", "-"},
    {"playback", "transport.prev_frame",      QT_TRANSLATE_NOOP("kk::StandardActions", "Previous Frame"), "prev_frame"},
    {"playback", "transport.next_frame",      QT_TRANSLATE_NOOP("kk::StandardActions", "Next Frame"), "next_frame"},
    {"playback", "transport.back_1s",         QT_TRANSLATE_NOOP("kk::StandardActions", "Back One Second")},
    {"playback", "transport.forward_1s",      QT_TRANSLATE_NOOP("kk::StandardActions", "Forward One Second")},
    {"playback", "-"},
    {"playback", "transport.prev_edit",       QT_TRANSLATE_NOOP("kk::StandardActions", "Previous Edit Point"), "prev_edit"},
    {"playback", "transport.next_edit",       QT_TRANSLATE_NOOP("kk::StandardActions", "Next Edit Point"), "next_edit"},
    {"playback", "transport.go_to_start",     QT_TRANSLATE_NOOP("kk::StandardActions", "Go to Start"), "go_start"},
    {"playback", "transport.go_to_end",       QT_TRANSLATE_NOOP("kk::StandardActions", "Go to End"), "go_end"},
    {"playback", "transport.prev_marker",     QT_TRANSLATE_NOOP("kk::StandardActions", "Previous Marker")},
    {"playback", "transport.next_marker",     QT_TRANSLATE_NOOP("kk::StandardActions", "Next Marker")},
    {"playback", "-"},
    {"playback", "transport.loop",            QT_TRANSLATE_NOOP("kk::StandardActions", "&Loop Playback"), "loop", true},
    {"playback", "transport.play_in_out",     QT_TRANSLATE_NOOP("kk::StandardActions", "Play &In to Out")},
    {"playback", "transport.play_around",     QT_TRANSLATE_NOOP("kk::StandardActions", "Play &Around Current Frame")},
    {"playback", "-"},
    // Resolve: Playback > Timeline Proxy Resolution (src/ui/viewer/PlaybackResolution).
    {"playback", ">playback.resolution",      QT_TRANSLATE_NOOP("kk::StandardActions", "Playback &Resolution")},
    {"playback.resolution", "playback.resolution_full",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Full Resolution"), "", true},
    {"playback.resolution", "playback.resolution_half",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Half Resolution"), "", true},
    {"playback.resolution", "playback.resolution_quarter", QT_TRANSLATE_NOOP("kk::StandardActions", "&Quarter Resolution"), "", true},
    {"playback.resolution", "-"},
    {"playback.resolution", "playback.resolution_auto",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Automatic (Half When Playback Cannot Keep Up)"), "", true},
    // Resolve: Playback > Proxy Handling, Use Optimized Media If Available, Render Cache.
    {"playback", ">playback.proxy_handling",  QT_TRANSLATE_NOOP("kk::StandardActions", "Pro&xy Handling")},
    {"playback.proxy_handling", "playback.proxy_prefer",    QT_TRANSLATE_NOOP("kk::StandardActions", "Prefer &Proxies"), "", true},
    {"playback.proxy_handling", "playback.proxy_originals", QT_TRANSLATE_NOOP("kk::StandardActions", "Prefer &Camera Originals"), "", true},
    {"playback.proxy_handling", "playback.proxy_disable",   QT_TRANSLATE_NOOP("kk::StandardActions", "&Disable All Proxies"), "", true},
    {"playback", "playback.use_proxy",        QT_TRANSLATE_NOOP("kk::StandardActions", "Use Pro&xy Media If Available"), "", true},
    {"playback", "playback.use_optimized",    QT_TRANSLATE_NOOP("kk::StandardActions", "Use &Optimized Media If Available"), "", true},
    {"playback", ">playback.render_cache",    QT_TRANSLATE_NOOP("kk::StandardActions", "Render &Cache")},
    {"playback.render_cache", "playback.render_cache_none",  QT_TRANSLATE_NOOP("kk::StandardActions", "&None"), "", true},
    {"playback.render_cache", "playback.render_cache_smart", QT_TRANSLATE_NOOP("kk::StandardActions", "&Smart"), "", true},
    {"playback.render_cache", "playback.render_cache_user",  QT_TRANSLATE_NOOP("kk::StandardActions", "&User"), "", true},
    {"playback", "playback.delete_render_cache", QT_TRANSLATE_NOOP("kk::StandardActions", "&Delete Render Cache…")},

    // ---- Media pool (File submenu + context menu; MediaPoolPanel puts the selected ids into
    // the data) ----
    // ---- Cut page (submenu of Edit) ----
    {"cut", "cut.smart_insert",       QT_TRANSLATE_NOOP("kk::StandardActions", "Smart &Insert"), "smart_insert"},
    {"cut", "cut.append",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Append"), "append"},
    {"cut", "cut.ripple_overwrite",   QT_TRANSLATE_NOOP("kk::StandardActions", "&Ripple Overwrite"), "ripple_overwrite"},
    {"cut", "cut.close_up",           QT_TRANSLATE_NOOP("kk::StandardActions", "&Close Up"), "close_up"},
    {"cut", "cut.place_on_top",       QT_TRANSLATE_NOOP("kk::StandardActions", "Place on &Top"), "place_on_top"},
    {"cut", "cut.source_overwrite",   QT_TRANSLATE_NOOP("kk::StandardActions", "&Source Overwrite"), "source_overwrite"},
    {"cut", "-"},
    {"cut", "cut.fast_review",        QT_TRANSLATE_NOOP("kk::StandardActions", "&Fast Review"), "fast_review", true},
    {"cut", "-"},
    {"cut", "cut.viewer_source_clip", QT_TRANSLATE_NOOP("kk::StandardActions", "Viewer: Source &Clip"), "source_clip", true},
    {"cut", "cut.viewer_source_tape", QT_TRANSLATE_NOOP("kk::StandardActions", "Viewer: Source Ta&pe"), "source_tape", true},
    {"cut", "cut.viewer_timeline",    QT_TRANSLATE_NOOP("kk::StandardActions", "Viewer: Time&line"), "timeline", true},

    // ---- Compositing page node editor (submenu of Edit; src/ui/comp, CompositingPage) ----
    {"comp", "comp.select_node",      QT_TRANSLATE_NOOP("kk::StandardActions", "&Add Node…"), "add"},
    {"comp", "comp.add_note",         QT_TRANSLATE_NOOP("kk::StandardActions", "Add N&ote")},
    {"comp", "-"},
    {"comp", "comp.cut",              QT_TRANSLATE_NOOP("kk::StandardActions", "Cu&t Nodes")},
    {"comp", "comp.copy",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Copy Nodes")},
    {"comp", "comp.paste",            QT_TRANSLATE_NOOP("kk::StandardActions", "&Paste Nodes")},
    {"comp", "comp.duplicate",        QT_TRANSLATE_NOOP("kk::StandardActions", "D&uplicate Nodes")},
    {"comp", "comp.delete",           QT_TRANSLATE_NOOP("kk::StandardActions", "&Delete Nodes")},
    {"comp", "comp.select_all",       QT_TRANSLATE_NOOP("kk::StandardActions", "Select &All Nodes")},
    {"comp", "-"},
    {"comp", "comp.rename",           QT_TRANSLATE_NOOP("kk::StandardActions", "&Rename Node")},
    {"comp", "comp.pass_through",     QT_TRANSLATE_NOOP("kk::StandardActions", "Pass &Through"), "", true},
    {"comp", "comp.lock",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Lock Node"), "lock", true},
    {"comp", "comp.swap_inputs",      QT_TRANSLATE_NOOP("kk::StandardActions", "&Swap Inputs")},
    {"comp", "-"},
    {"comp", "comp.view_left",        QT_TRANSLATE_NOOP("kk::StandardActions", "View in &Left Viewer")},
    {"comp", "comp.view_right",       QT_TRANSLATE_NOOP("kk::StandardActions", "View in &Right Viewer")},
    {"comp", "comp.fit",              QT_TRANSLATE_NOOP("kk::StandardActions", "&Fit Nodes to View")},
    {"comp", "comp.snap_grid",        QT_TRANSLATE_NOOP("kk::StandardActions", "Snap Nodes to &Grid"), "snap", true},

    // ---- Color page (submenu of Edit; src/ui/color, ColorPage) ----
    {"color", "color.add_serial",     QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Serial Node"), "add"},
    {"color", "color.add_parallel",   QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Parallel Node")},
    {"color", "color.add_layer",      QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Layer Node")},
    {"color", "color.add_serial_before", QT_TRANSLATE_NOOP("kk::StandardActions", "Add Serial Node &Before")},
    {"color", "color.delete_node",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Delete Node")},
    {"color", "-"},
    {"color", "color.bypass_node",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Enable / Disable Current Node")},
    {"color", "color.bypass_all",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Bypass All Grades"), "", true},
    {"color", "color.reset_node",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Reset Node Grade")},
    {"color", "color.reset_grade",    QT_TRANSLATE_NOOP("kk::StandardActions", "Reset &Clip Grade")},
    {"color", "color.timeline_grade", QT_TRANSLATE_NOOP("kk::StandardActions", "Grade the &Timeline"), "", true},
    {"color", "color.keyframe",       QT_TRANSLATE_NOOP("kk::StandardActions", "Add / Remove &Keyframe"), "keyframe_on"},
    {"color", "-"},
    {"color", "color.grab_still",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Grab Still")},
    {"color", "color.apply_still",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Apply Grade from Still")},
    {"color", "color.wipe",           QT_TRANSLATE_NOOP("kk::StandardActions", "&Wipe with Still"), "", true},
    {"color", "color.highlight",      QT_TRANSLATE_NOOP("kk::StandardActions", "&Highlight"), "", true},
    {"color", "-"},
    {"color", "color.copy_grade",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Copy Grade")},
    {"color", "color.paste_grade",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Paste Grade")},

    // ---- Audio page / mixer (own "Audio" menu) ----
    {"audio", "audio.add_bus",          QT_TRANSLATE_NOOP("kk::StandardActions", "Add &Bus"), "add"},
    {"audio", "audio.reset_loudness",   QT_TRANSLATE_NOOP("kk::StandardActions", "&Reset Loudness Meter")},
    {"audio", "-"},
    {"audio", "audio.float_viewer",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Float Viewer"), "", true},

    {"mediapool", "mediapool.generate_optimized", QT_TRANSLATE_NOOP("kk::StandardActions", "Generate Optimized Media")},
    {"mediapool", "mediapool.generate_proxy",     QT_TRANSLATE_NOOP("kk::StandardActions", "Generate Proxy Media")},
    {"mediapool", "mediapool.stop_generation",    QT_TRANSLATE_NOOP("kk::StandardActions", "Stop Generating")},
    {"mediapool", "mediapool.link_proxy",         QT_TRANSLATE_NOOP("kk::StandardActions", "Link Proxy Media…")},
    {"mediapool", "mediapool.unlink_proxy",       QT_TRANSLATE_NOOP("kk::StandardActions", "Unlink Proxy Media")},
    {"mediapool", "mediapool.delete_proxy",       QT_TRANSLATE_NOOP("kk::StandardActions", "Delete Proxy Media…")},
    {"mediapool", "mediapool.delete_optimized",   QT_TRANSLATE_NOOP("kk::StandardActions", "Delete Optimized Media…")},

    // ---- Media pool organisation (File > Media Pool; MediaPoolPanel acts on its selection) ----
    {"mediapool.organize", "mediapool.select_in_timeline", QT_TRANSLATE_NOOP("kk::StandardActions", "Select Clips in &Timeline")},
    {"mediapool.organize", "mediapool.remove_unused", QT_TRANSLATE_NOOP("kk::StandardActions", "Remove &Unused Clips…")},

    // ---- Settings (look and feel; entries filled by src/app/Theme) ----
    {"settings", ">settings.color_scheme", QT_TRANSLATE_NOOP("kk::StandardActions", "&Color Theme")},
    {"settings", ">settings.style",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Style")},

    // ---- Help ----
    // In-app help (src/ui/help): links to the user guide in docs/guide.
    {"help", "help.guide_context",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Help for This…")},
    {"help", "help.guide",            QT_TRANSLATE_NOOP("kk::StandardActions", "&Guide…")},
    {"help", "help.getting_started",  QT_TRANSLATE_NOOP("kk::StandardActions", "Getting &Started")},
    {"help", "-"},
    {"help", "help.about",            QT_TRANSLATE_NOOP("kk::StandardActions", "&About %1")},
    {"help", "help.about_qt",         QT_TRANSLATE_NOOP("kk::StandardActions", "About &Qt")},
};
// clang-format on

/// Number of tracks with destination / auto-select shortcuts (V1-V8, A1-A8).
constexpr int kTrackShortcutCount = 8;
/// Angles that have a "Cut / Switch to Angle n" command (number keys 1-9).
constexpr int kMulticamAngleShortcutCount = 9;

QString translate(const char* text)
{
    return QCoreApplication::translate(kContext, text);
}

} // namespace

QVector<MenuCategory> standardMenus()
{
    return {
        {QStringLiteral("file"), translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&File"))},
        {QStringLiteral("edit"), translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&Edit"))},
        {QStringLiteral("trim"), translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&Trim"))},
        {QStringLiteral("timeline"),
            translate(QT_TRANSLATE_NOOP("kk::StandardActions", "Time&line"))},
        {QStringLiteral("clip"), translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&Clip"))},
        {QStringLiteral("mark"), translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&Mark"))},
        {QStringLiteral("view"), translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&View"))},
        {QStringLiteral("playback"),
            translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&Playback"))},
        {QStringLiteral("audio"), translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&Audio"))},
        {QStringLiteral("settings"),
            translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&Settings"))},
        {QStringLiteral("help"), translate(QT_TRANSLATE_NOOP("kk::StandardActions", "&Help"))},
    };
}

void registerStandardActions(ActionRegistry& registry)
{
    for (const ActionSpec& spec : kActions) {
        const QString category = QLatin1String(spec.category);
        if (qstrcmp(spec.id, ActionRegistry::kSeparator) == 0) {
            registry.addSeparator(category);
            continue;
        }
        if (const QString sub = ActionRegistry::submenuOf(QLatin1String(spec.id)); !sub.isEmpty()) {
            registry.addSubmenu(category, sub, translate(spec.text));
            continue;
        }
        registry.add(QLatin1String(spec.id), translate(spec.text), category,
            QLatin1String(spec.icon), spec.checkable);
    }

    // Initial states and groupings that are part of the command's definition.
    QAction* about = registry.action(QStringLiteral("help.about"));
    about->setText(about->text().arg(QCoreApplication::applicationName()));
    about->setMenuRole(QAction::AboutRole);
    registry.action(QStringLiteral("help.about_qt"))->setMenuRole(QAction::AboutQtRole);
    registry.action(QStringLiteral("file.quit"))->setMenuRole(QAction::QuitRole);
    registry.action(QStringLiteral("edit.preferences"))->setMenuRole(QAction::PreferencesRole);

    auto* tools = new QActionGroup(&registry);
    for (const char* id : {"tool.select", "tool.trim", "tool.blade"})
        tools->addAction(registry.action(QLatin1String(id)));
    registry.action(QStringLiteral("tool.select"))->setChecked(true);
    auto* resolutions = new QActionGroup(&registry);
    for (const char* id : {"playback.resolution_full", "playback.resolution_half",
             "playback.resolution_quarter", "playback.resolution_auto"})
        resolutions->addAction(registry.action(QLatin1String(id)));
    registry.action(QStringLiteral("playback.resolution_full"))->setChecked(true);
    for (const auto& group : {std::initializer_list<const char*> {"playback.proxy_prefer",
                                  "playback.proxy_originals", "playback.proxy_disable"},
             std::initializer_list<const char*> {"playback.render_cache_none",
                 "playback.render_cache_smart", "playback.render_cache_user"}}) {
        auto* g = new QActionGroup(&registry);
        for (const char* id : group)
            g->addAction(registry.action(QLatin1String(id)));
    }
    registry.action(QStringLiteral("playback.proxy_prefer"))->setChecked(true);
    registry.action(QStringLiteral("playback.use_proxy"))->setChecked(true);
    registry.action(QStringLiteral("playback.use_optimized"))->setChecked(true);
    registry.action(QStringLiteral("playback.render_cache_none"))->setChecked(true);
    registry.action(QStringLiteral("timeline.snapping"))->setChecked(true);
    auto* multicamModes = new QActionGroup(&registry);
    for (const char* id : {"multicam.mode_cut", "multicam.mode_switch"})
        multicamModes->addAction(registry.action(QLatin1String(id)));
    registry.action(QStringLiteral("multicam.mode_cut"))->setChecked(true);
    auto* multicamTargets = new QActionGroup(&registry);
    for (const char* id :
        {"multicam.target_both", "multicam.target_video", "multicam.target_audio"})
        multicamTargets->addAction(registry.action(QLatin1String(id)));
    registry.action(QStringLiteral("multicam.target_both"))->setChecked(true);
    // Number keys: cut (or switch, per the mode) to angle 1-9 of the multicam clip under the
    // timeline playhead while the multicam viewer is shown.
    for (int n = 1; n <= kMulticamAngleShortcutCount; ++n) {
        registry.add(QStringLiteral("multicam.angle_") + QString::number(n),
            translate(QT_TRANSLATE_NOOP("kk::StandardActions", "Cut / Switch to Angle %1")).arg(n),
            QStringLiteral("clip.multicam"));
    }
    registry.action(QStringLiteral("timeline.linked_selection"))->setChecked(true);

    // Short labels for the page bar buttons (menus keep the full "Media Page" text).
    const char* const pageLabels[][2] = {
        {"view.page_media", QT_TRANSLATE_NOOP("kk::StandardActions", "Media")},
        {"view.page_cut", QT_TRANSLATE_NOOP("kk::StandardActions", "Cut")},
        {"view.page_edit", QT_TRANSLATE_NOOP("kk::StandardActions", "Edit")},
        {"view.page_compositing", QT_TRANSLATE_NOOP("kk::StandardActions", "Compositing")},
        {"view.page_color", QT_TRANSLATE_NOOP("kk::StandardActions", "Color")},
        {"view.page_audio", QT_TRANSLATE_NOOP("kk::StandardActions", "Audio")},
        {"view.page_deliver", QT_TRANSLATE_NOOP("kk::StandardActions", "Deliver")},
    };
    for (const auto& [id, label] : pageLabels)
        registry.action(QLatin1String(id))->setIconText(translate(label));

    // Track destination (patch) and auto track selector toggles per track number (data-driven
    // submenu; keys in the keymap profile).
    for (int n = 1; n <= kTrackShortcutCount; ++n) {
        const QString num = QString::number(n);
        registry.add(QStringLiteral("timeline.target_video_") + num,
            translate(QT_TRANSLATE_NOOP("kk::StandardActions", "Video Destination V%1")).arg(n),
            QStringLiteral("timeline.tracks"));
    }
    registry.addSeparator(QStringLiteral("timeline.tracks"));
    for (int n = 1; n <= kTrackShortcutCount; ++n) {
        const QString num = QString::number(n);
        registry.add(QStringLiteral("timeline.target_audio_") + num,
            translate(QT_TRANSLATE_NOOP("kk::StandardActions", "Audio Destination A%1")).arg(n),
            QStringLiteral("timeline.tracks"));
    }
    registry.addSeparator(QStringLiteral("timeline.tracks"));
    for (int n = 1; n <= kTrackShortcutCount; ++n) {
        const QString num = QString::number(n);
        registry.add(QStringLiteral("timeline.auto_select_video_") + num,
            translate(QT_TRANSLATE_NOOP("kk::StandardActions", "Toggle Auto Select V%1")).arg(n),
            QStringLiteral("timeline.tracks"));
    }
    registry.addSeparator(QStringLiteral("timeline.tracks"));
    for (int n = 1; n <= kTrackShortcutCount; ++n) {
        const QString num = QString::number(n);
        registry.add(QStringLiteral("timeline.auto_select_audio_") + num,
            translate(QT_TRANSLATE_NOOP("kk::StandardActions", "Toggle Auto Select A%1")).arg(n),
            QStringLiteral("timeline.tracks"));
    }

    // Pages of the Resolve layout that are not built yet (visible in the page bar, so the bar
    // has Resolve's order and proportions) and commands of subsystems that do not exist yet
    // (audio scrubbing): present so the keymap matches Resolve's, but
    // disabled.
    auto* cutViewerModes = new QActionGroup(&registry);
    for (const char* id :
        {"cut.viewer_source_clip", "cut.viewer_source_tape", "cut.viewer_timeline"})
        cutViewerModes->addAction(registry.action(QLatin1String(id)));
    registry.action(QStringLiteral("cut.viewer_timeline"))->setChecked(true);
    // The Cut page enables its commands while it is shown (CutPage::showEvent()).
    for (const QString& id : registry.menuEntries(QStringLiteral("cut"))) {
        if (registry.contains(id)) {
            registry.action(id)->setEnabled(false);
            registry.setShortcutScope(id, QStringLiteral("page:cut"));
        }
    }

    // Node editor keys act only while the node editor has focus (NodeEditor claims them through
    // QEvent::ShortcutOverride); as window shortcuts they would clash with Copy, Delete, the
    // multicam number keys and so on. Their scope tells Keyboard Customization that sharing a
    // key with an Edit page command is no conflict.
    for (const QString& id : registry.menuEntries(QStringLiteral("comp"))) {
        if (registry.contains(id)) {
            registry.action(id)->setShortcutContext(Qt::WidgetShortcut);
            registry.setShortcutScope(id, QStringLiteral("widget:node_editor"));
        }
    }

    // Color page keys act while the page is shown and has focus (ColorPage claims them
    // through QEvent::ShortcutOverride, like the node editor); as window shortcuts Ctrl+D and
    // the Alt keys would clash with Edit page commands.
    for (const QString& id : registry.menuEntries(QStringLiteral("color"))) {
        if (registry.contains(id)) {
            registry.action(id)->setShortcutContext(Qt::WidgetShortcut);
            registry.setShortcutScope(id, QStringLiteral("widget:color_page"));
        }
    }
}

} // namespace kk
