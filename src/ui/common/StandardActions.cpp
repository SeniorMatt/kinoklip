// SPDX-License-Identifier: GPL-3.0-or-later
// Table of every standard command. Default shortcuts live in resources/keymap_default.json.
#include "ui/common/StandardActions.h"

#include "ui/common/ActionRegistry.h"

#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>

namespace cw {

namespace {

const char* const kContext = "cw::StandardActions";

/// One row of the catalogue. id "-" = separator in the current category.
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
    {"file", "file.new",              QT_TRANSLATE_NOOP("cw::StandardActions", "&New Project")},
    {"file", "file.open",             QT_TRANSLATE_NOOP("cw::StandardActions", "&Open Project…")},
    {"file", "-"},
    {"file", "file.save",             QT_TRANSLATE_NOOP("cw::StandardActions", "&Save Project")},
    {"file", "file.save_as",          QT_TRANSLATE_NOOP("cw::StandardActions", "Save Project &As…")},
    {"file", "-"},
    {"file", "file.import_media",     QT_TRANSLATE_NOOP("cw::StandardActions", "&Import Media…"), "import"},
    {"file", "-"},
    {"file", "file.project_settings", QT_TRANSLATE_NOOP("cw::StandardActions", "Project Se&ttings…")},
    {"file", "-"},
    {"file", "file.quit",             QT_TRANSLATE_NOOP("cw::StandardActions", "&Quit")},

    // ---- Edit ----
    {"edit", "edit.undo",             QT_TRANSLATE_NOOP("cw::StandardActions", "&Undo")},
    {"edit", "edit.redo",             QT_TRANSLATE_NOOP("cw::StandardActions", "&Redo")},
    {"edit", "-"},
    {"edit", "edit.cut",              QT_TRANSLATE_NOOP("cw::StandardActions", "Cu&t")},
    {"edit", "edit.copy",             QT_TRANSLATE_NOOP("cw::StandardActions", "&Copy")},
    {"edit", "edit.paste",            QT_TRANSLATE_NOOP("cw::StandardActions", "&Paste")},
    {"edit", "-"},
    {"edit", "edit.delete",           QT_TRANSLATE_NOOP("cw::StandardActions", "&Delete")},
    {"edit", "edit.ripple_delete",    QT_TRANSLATE_NOOP("cw::StandardActions", "Ripple De&lete")},
    {"edit", "-"},
    {"edit", "edit.select_all",       QT_TRANSLATE_NOOP("cw::StandardActions", "Select &All")},
    {"edit", "edit.deselect_all",     QT_TRANSLATE_NOOP("cw::StandardActions", "Deselect All")},
    {"edit", "-"},
    {"edit", "edit.insert",           QT_TRANSLATE_NOOP("cw::StandardActions", "&Insert Clip"), "insert"},
    {"edit", "edit.overwrite",        QT_TRANSLATE_NOOP("cw::StandardActions", "&Overwrite Clip"), "overwrite"},
    {"edit", "edit.place_on_top",     QT_TRANSLATE_NOOP("cw::StandardActions", "Place on &Top"), "place_on_top"},
    {"edit", "-"},
    {"edit", "edit.keyboard_customization", QT_TRANSLATE_NOOP("cw::StandardActions", "&Keyboard Customization…")},
    {"edit", "edit.preferences",      QT_TRANSLATE_NOOP("cw::StandardActions", "Pr&eferences…")},

    // ---- Trim (edit tools; the three tools form an exclusive group) ----
    {"trim", "tool.select",           QT_TRANSLATE_NOOP("cw::StandardActions", "&Selection Mode"), "select", true},
    {"trim", "tool.trim",             QT_TRANSLATE_NOOP("cw::StandardActions", "&Trim Edit Mode"), "trim", true},
    {"trim", "tool.blade",            QT_TRANSLATE_NOOP("cw::StandardActions", "&Blade Edit Mode"), "blade", true},
    {"trim", "-"},
    {"trim", "trim.nudge_left",       QT_TRANSLATE_NOOP("cw::StandardActions", "Nudge One Frame &Left")},
    {"trim", "trim.nudge_right",      QT_TRANSLATE_NOOP("cw::StandardActions", "Nudge One Frame &Right")},

    // ---- Timeline ----
    {"timeline", "edit.split",        QT_TRANSLATE_NOOP("cw::StandardActions", "&Split Clip at Playhead"), "split"},
    {"timeline", "timeline.add_transition", QT_TRANSLATE_NOOP("cw::StandardActions", "Add Default &Transition")},
    {"timeline", "-"},
    {"timeline", "timeline.snapping", QT_TRANSLATE_NOOP("cw::StandardActions", "S&napping"), "snap", true},
    {"timeline", "timeline.linked_selection", QT_TRANSLATE_NOOP("cw::StandardActions", "&Linked Selection"), "link", true},
    {"timeline", "-"},
    {"timeline", "timeline.zoom_in",  QT_TRANSLATE_NOOP("cw::StandardActions", "Zoom &In"), "zoom_in"},
    {"timeline", "timeline.zoom_out", QT_TRANSLATE_NOOP("cw::StandardActions", "Zoom &Out"), "zoom_out"},
    {"timeline", "timeline.zoom_fit", QT_TRANSLATE_NOOP("cw::StandardActions", "Zoom to &Fit"), "zoom_fit"},
    {"timeline", "-"},
    {"timeline", "timeline.add_video_track", QT_TRANSLATE_NOOP("cw::StandardActions", "Add &Video Track")},
    {"timeline", "timeline.add_audio_track", QT_TRANSLATE_NOOP("cw::StandardActions", "Add &Audio Track")},

    // ---- Clip ----
    {"clip", "clip.toggle_enabled",   QT_TRANSLATE_NOOP("cw::StandardActions", "&Enable / Disable Clip")},
    {"clip", "-"},
    {"clip", "clip.link",             QT_TRANSLATE_NOOP("cw::StandardActions", "&Link Clips")},
    {"clip", "clip.unlink",           QT_TRANSLATE_NOOP("cw::StandardActions", "&Unlink Clips")},
    {"clip", "-"},
    {"clip", "clip.add_keyframe",     QT_TRANSLATE_NOOP("cw::StandardActions", "Add &Keyframe"), "keyframe_on"},
    {"clip", "clip.prev_keyframe",    QT_TRANSLATE_NOOP("cw::StandardActions", "&Previous Keyframe"), "keyframe_prev"},
    {"clip", "clip.next_keyframe",    QT_TRANSLATE_NOOP("cw::StandardActions", "&Next Keyframe"), "keyframe_next"},

    // ---- Mark ----
    {"mark", "mark.in",               QT_TRANSLATE_NOOP("cw::StandardActions", "Mark &In"), "mark_in"},
    {"mark", "mark.out",              QT_TRANSLATE_NOOP("cw::StandardActions", "Mark &Out"), "mark_out"},
    {"mark", "mark.clip",             QT_TRANSLATE_NOOP("cw::StandardActions", "Mark &Clip")},
    {"mark", "-"},
    {"mark", "mark.clear_in",         QT_TRANSLATE_NOOP("cw::StandardActions", "Clear In")},
    {"mark", "mark.clear_out",        QT_TRANSLATE_NOOP("cw::StandardActions", "Clear Out")},
    {"mark", "mark.clear_both",       QT_TRANSLATE_NOOP("cw::StandardActions", "Clear In and Out")},
    {"mark", "-"},
    {"mark", "mark.go_to_in",         QT_TRANSLATE_NOOP("cw::StandardActions", "Go to In")},
    {"mark", "mark.go_to_out",        QT_TRANSLATE_NOOP("cw::StandardActions", "Go to Out")},
    {"mark", "-"},
    {"mark", "mark.add_marker",       QT_TRANSLATE_NOOP("cw::StandardActions", "Add &Marker"), "marker"},

    // ---- View ----
    {"view", "view.page_media",       QT_TRANSLATE_NOOP("cw::StandardActions", "&Media Page"), "page_media", true},
    {"view", "view.page_edit",        QT_TRANSLATE_NOOP("cw::StandardActions", "&Edit Page"), "page_edit", true},
    {"view", "view.page_color",       QT_TRANSLATE_NOOP("cw::StandardActions", "&Color Page"), "page_color", true},
    {"view", "view.page_deliver",     QT_TRANSLATE_NOOP("cw::StandardActions", "&Deliver Page"), "page_deliver", true},
    {"view", "-"},
    {"view", "view.toggle_media_pool", QT_TRANSLATE_NOOP("cw::StandardActions", "Media Pool"), "panel_media_pool", true},
    {"view", "view.toggle_effects",   QT_TRANSLATE_NOOP("cw::StandardActions", "Effects"), "panel_effects", true},
    {"view", "view.toggle_edit_index", QT_TRANSLATE_NOOP("cw::StandardActions", "Edit Index"), "panel_edit_index", true},
    {"view", "view.toggle_mixer",     QT_TRANSLATE_NOOP("cw::StandardActions", "Mixer"), "panel_mixer", true},
    {"view", "view.toggle_metadata",  QT_TRANSLATE_NOOP("cw::StandardActions", "Metadata"), "panel_metadata", true},
    {"view", "view.toggle_inspector", QT_TRANSLATE_NOOP("cw::StandardActions", "Inspector"), "panel_inspector", true},
    {"view", "-"},
    {"view", "view.single_viewer",    QT_TRANSLATE_NOOP("cw::StandardActions", "&Single Viewer Mode"), "single_viewer", true},
    {"view", "view.fullscreen_viewer", QT_TRANSLATE_NOOP("cw::StandardActions", "Full-Screen &Viewer"), "fullscreen"},
    {"view", "view.fullscreen",       QT_TRANSLATE_NOOP("cw::StandardActions", "&Full Screen"), "", true},
    {"view", "-"},
    {"view", "view.use_system_colors", QT_TRANSLATE_NOOP("cw::StandardActions", "Use System &Colors"), "", true},

    // ---- Playback ----
    {"playback", "transport.play_pause",      QT_TRANSLATE_NOOP("cw::StandardActions", "&Play / Pause"), "play"},
    {"playback", "transport.shuttle_reverse", QT_TRANSLATE_NOOP("cw::StandardActions", "Shuttle &Reverse")},
    {"playback", "transport.shuttle_stop",    QT_TRANSLATE_NOOP("cw::StandardActions", "&Stop"), "stop"},
    {"playback", "transport.shuttle_forward", QT_TRANSLATE_NOOP("cw::StandardActions", "Shuttle &Forward")},
    {"playback", "-"},
    {"playback", "transport.prev_frame",      QT_TRANSLATE_NOOP("cw::StandardActions", "Previous Frame"), "prev_frame"},
    {"playback", "transport.next_frame",      QT_TRANSLATE_NOOP("cw::StandardActions", "Next Frame"), "next_frame"},
    {"playback", "transport.back_1s",         QT_TRANSLATE_NOOP("cw::StandardActions", "Back One Second")},
    {"playback", "transport.forward_1s",      QT_TRANSLATE_NOOP("cw::StandardActions", "Forward One Second")},
    {"playback", "-"},
    {"playback", "transport.prev_edit",       QT_TRANSLATE_NOOP("cw::StandardActions", "Previous Edit Point"), "prev_edit"},
    {"playback", "transport.next_edit",       QT_TRANSLATE_NOOP("cw::StandardActions", "Next Edit Point"), "next_edit"},
    {"playback", "transport.go_to_start",     QT_TRANSLATE_NOOP("cw::StandardActions", "Go to Start"), "go_start"},
    {"playback", "transport.go_to_end",       QT_TRANSLATE_NOOP("cw::StandardActions", "Go to End"), "go_end"},
    {"playback", "-"},
    {"playback", "transport.loop",            QT_TRANSLATE_NOOP("cw::StandardActions", "&Loop Playback"), "loop", true},

    // ---- Help ----
    {"help", "help.about",            QT_TRANSLATE_NOOP("cw::StandardActions", "&About %1")},
    {"help", "help.about_qt",         QT_TRANSLATE_NOOP("cw::StandardActions", "About &Qt")},
};
// clang-format on

QString translate(const char* text)
{
    return QCoreApplication::translate(kContext, text);
}

} // namespace

QVector<MenuCategory> standardMenus()
{
    return {
        {QStringLiteral("file"), translate(QT_TRANSLATE_NOOP("cw::StandardActions", "&File"))},
        {QStringLiteral("edit"), translate(QT_TRANSLATE_NOOP("cw::StandardActions", "&Edit"))},
        {QStringLiteral("trim"), translate(QT_TRANSLATE_NOOP("cw::StandardActions", "&Trim"))},
        {QStringLiteral("timeline"),
            translate(QT_TRANSLATE_NOOP("cw::StandardActions", "Time&line"))},
        {QStringLiteral("clip"), translate(QT_TRANSLATE_NOOP("cw::StandardActions", "&Clip"))},
        {QStringLiteral("mark"), translate(QT_TRANSLATE_NOOP("cw::StandardActions", "&Mark"))},
        {QStringLiteral("view"), translate(QT_TRANSLATE_NOOP("cw::StandardActions", "&View"))},
        {QStringLiteral("playback"),
            translate(QT_TRANSLATE_NOOP("cw::StandardActions", "&Playback"))},
        {QStringLiteral("help"), translate(QT_TRANSLATE_NOOP("cw::StandardActions", "&Help"))},
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
    registry.action(QStringLiteral("timeline.snapping"))->setChecked(true);
    registry.action(QStringLiteral("timeline.linked_selection"))->setChecked(true);

    // Short labels for the page bar buttons (menus keep the full "Media Page" text).
    const char* const pageLabels[][2] = {
        {"view.page_media", QT_TRANSLATE_NOOP("cw::StandardActions", "Media")},
        {"view.page_edit", QT_TRANSLATE_NOOP("cw::StandardActions", "Edit")},
        {"view.page_color", QT_TRANSLATE_NOOP("cw::StandardActions", "Color")},
        {"view.page_deliver", QT_TRANSLATE_NOOP("cw::StandardActions", "Deliver")},
    };
    for (const auto& [id, label] : pageLabels)
        registry.action(QLatin1String(id))->setIconText(translate(label));

    QAction* color = registry.action(QStringLiteral("view.page_color"));
    color->setEnabled(false);
    color->setStatusTip(translate(QT_TRANSLATE_NOOP("cw::StandardActions", "Coming later")));
}

} // namespace cw
