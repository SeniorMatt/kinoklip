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
    {"file", "file.new",              QT_TRANSLATE_NOOP("kk::StandardActions", "&New Project")},
    {"file", "timeline.new",          QT_TRANSLATE_NOOP("kk::StandardActions", "New &Timeline…"), "timeline"},
    {"file", "file.open",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Open Project…")},
    {"file", ">file.recent",          QT_TRANSLATE_NOOP("kk::StandardActions", "Open &Recent")},
    {"file", "-"},
    {"file", "file.save",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Save Project")},
    {"file", "file.save_as",          QT_TRANSLATE_NOOP("kk::StandardActions", "Save Project &As…")},
    {"file", "-"},
    {"file", "file.import_media",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Import Media…"), "import"},
    {"file", "-"},
    {"file", "file.project_settings", QT_TRANSLATE_NOOP("kk::StandardActions", "Project Se&ttings…")},
    {"file", "-"},
    {"file", "file.quit",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Quit")},

    // ---- Edit ----
    {"edit", "edit.undo",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Undo")},
    {"edit", "edit.redo",             QT_TRANSLATE_NOOP("kk::StandardActions", "&Redo")},
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
    {"clip", "clip.match_frame",      QT_TRANSLATE_NOOP("kk::StandardActions", "&Match Frame")},
    {"clip", "-"},
    {"clip", "clip.change_duration",  QT_TRANSLATE_NOOP("kk::StandardActions", "Change Clip &Duration…")},
    {"clip", "clip.change_speed",     QT_TRANSLATE_NOOP("kk::StandardActions", "Change Clip &Speed…")},
    {"clip", "clip.retime_controls",  QT_TRANSLATE_NOOP("kk::StandardActions", "&Retime Controls")},
    {"clip", "clip.freeze_frame",     QT_TRANSLATE_NOOP("kk::StandardActions", "&Freeze Frame")},
    {"clip", "-"},
    {"clip", "clip.new_compound",     QT_TRANSLATE_NOOP("kk::StandardActions", "New &Compound Clip…"), "compound_clip"},
    {"clip", "clip.open_in_timeline", QT_TRANSLATE_NOOP("kk::StandardActions", "&Open in Timeline")},
    {"clip", "clip.decompose",        QT_TRANSLATE_NOOP("kk::StandardActions", "Decompose in &Place")},
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
    {"view", "view.toggle_mixer",     QT_TRANSLATE_NOOP("kk::StandardActions", "Mixer"), "panel_mixer", true},
    {"view", "view.toggle_metadata",  QT_TRANSLATE_NOOP("kk::StandardActions", "Metadata"), "panel_metadata", true},
    {"view", "view.toggle_inspector", QT_TRANSLATE_NOOP("kk::StandardActions", "Inspector"), "panel_inspector", true},
    {"view", "-"},
    {"view", "view.single_viewer",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Single Viewer Mode"), "single_viewer", true},
    {"view", "view.toggle_source_timeline", QT_TRANSLATE_NOOP("kk::StandardActions", "Toggle Source / Timeline Viewer")},
    {"view", "view.fullscreen_viewer", QT_TRANSLATE_NOOP("kk::StandardActions", "Full-Screen &Viewer"), "fullscreen"},
    {"view", "view.onscreen_masks",   QT_TRANSLATE_NOOP("kk::StandardActions", "On-Screen &Mask Controls"), "", true},
    {"view", "view.fullscreen",       QT_TRANSLATE_NOOP("kk::StandardActions", "&Full Screen"), "", true},

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

    // ---- Settings (look and feel; entries filled by src/app/Theme) ----
    {"settings", ">settings.color_scheme", QT_TRANSLATE_NOOP("kk::StandardActions", "&Color Theme")},
    {"settings", ">settings.style",    QT_TRANSLATE_NOOP("kk::StandardActions", "&Style")},

    // ---- Help ----
    {"help", "help.about",            QT_TRANSLATE_NOOP("kk::StandardActions", "&About %1")},
    {"help", "help.about_qt",         QT_TRANSLATE_NOOP("kk::StandardActions", "About &Qt")},
};
// clang-format on

/// Number of tracks with destination / auto-select shortcuts (V1-V8, A1-A8).
constexpr int kTrackShortcutCount = 8;

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
    registry.action(QStringLiteral("timeline.snapping"))->setChecked(true);
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
    // (retime, freeze frames, audio scrubbing): present so the keymap matches Resolve's, but
    // disabled.
    for (const char* id :
        {"view.page_cut", "view.page_compositing", "view.page_color", "view.page_audio",
            "clip.retime_controls", "clip.freeze_frame", "timeline.audio_scrubbing"}) {
        QAction* a = registry.action(QLatin1String(id));
        a->setEnabled(false);
        a->setStatusTip(
            translate(QT_TRANSLATE_NOOP("kk::StandardActions", "Coming in a later version")));
    }
}

} // namespace kk
