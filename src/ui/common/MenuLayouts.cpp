// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/MenuLayouts.h"

#include "ui/common/ActionRegistry.h"

#include <QAction>
#include <QCoreApplication>
#include <QKeySequence>
#include <QMenu>
#include <QSettings>

namespace kk::menulayout {

namespace {

constexpr char kMirrorProperty[] = "kkMirrorOf";

QString T(const char* text)
{
    return QCoreApplication::translate("kk::MenuLayouts", text);
}

using Nodes = QList<Node>;

/// A command, with the menu text of the action itself or @p label.
Node A(const char* id, const char* label = nullptr)
{
    return {Node::Kind::Action, QLatin1String(id), label ? T(label) : QString(), {}};
}
Node S()
{
    return {Node::Kind::Separator, {}, {}, {}};
}
Node M(const char* title, Nodes children)
{
    return {Node::Kind::Submenu, {}, T(title), std::move(children)};
}
Node C(const char* category, const char* title)
{
    return {Node::Kind::Category, QLatin1String(category), T(title), {}};
}

Menu menu(const char* id, const char* title, Nodes entries)
{
    return {QLatin1String(id), T(title), std::move(entries)};
}

// clang-format off
Menu appMenu()
{
    return menu("app", "Kinoklip", {
        A("help.about"),
        S(),
        A("edit.preferences"),
        A("edit.keyboard_customization", "Keyboard Shortcuts…"),
        A("settings.vim_mode"),
        A("settings.reload_fonts"),
        S(),
        A("file.quit"),
    });
}

Menu fileMenu()
{
    return menu("file", "&File", {
        A("file.new"),
        A("file.open", "Open Project…"),
        C("file.recent", "Open Recent"),
        A("file.project_manager"),
        S(),
        A("file.save", "Save"),
        A("file.save_as", "Save As…"),
        S(),
        M("Import", {
            A("file.import_media", "Media…"),
            A("file.import_media_folder", "Media Folder…"),
            A("file.import_timeline", "Timeline…"),
            A("file.import_subtitle", "Subtitles…"),
        }),
        M("Export", {
            A("file.export_edl", "Timeline as EDL…"),
            A("file.export_otio", "Timeline as OpenTimelineIO…"),
            S(),
            A("file.export_markers_edl", "Markers as EDL…"),
            A("file.export_markers_csv", "Markers as CSV…"),
        }),
        M("Project Tools", {
            A("file.pack_project", "Pack…"),
            A("file.unpack_project", "Unpack…"),
            A("file.compact_project", "Compact…"),
            A("file.verify_project", "Verify Project File"),
            S(),
            A("mediapool.remove_unused", "Remove Unused Clips…"),
            S(),
            M("Proxies and Optimized Media", {
                A("mediapool.generate_proxy", "Generate Proxies"),
                A("mediapool.generate_optimized", "Generate Optimized Media"),
                A("mediapool.stop_generation", "Stop"),
                S(),
                A("mediapool.link_proxy", "Link Proxies…"),
                A("mediapool.unlink_proxy", "Unlink Proxies"),
                S(),
                A("mediapool.delete_proxy", "Delete Proxies…"),
                A("mediapool.delete_optimized", "Delete Optimized Media…"),
            }),
        }),
        A("file.project_settings"),
    });
}

Menu editMenu()
{
    return menu("edit", "&Edit", {
        A("edit.undo"),
        A("edit.redo"),
        A("edit.history_window", "History…"),
        S(),
        A("edit.cut"),
        A("edit.ripple_cut", "Ripple Cut"),
        A("edit.copy"),
        A("edit.paste"),
        M("Paste Special", {
            A("edit.paste_insert", "Paste Insert"),
            A("edit.paste_attributes", "Paste Attributes…"),
        }),
        S(),
        A("edit.delete", "Delete"),
        A("edit.ripple_delete", "Ripple Delete"),
        A("edit.delete_gaps", "Close Gaps"),
        S(),
        M("Select", {
            A("edit.select_all", "All"),
            A("edit.deselect_all", "None"),
            S(),
            A("edit.select_forward", "Forward"),
            A("edit.select_backward", "Backward"),
            A("edit.select_forward_all", "Forward on All Tracks"),
            A("edit.select_backward_all", "Backward on All Tracks"),
            S(),
            A("trim.select_nearest_clip", "Nearest Clip or Gap"),
        }),
    });
}

Menu viewMenu()
{
    return menu("view", "&View", {
        M("Go to Page", {
            A("view.page_media", "Media"),
            A("view.page_cut", "Cut"),
            A("view.page_edit", "Edit"),
            S(),
            A("view.page_compositing", "Compositing"),
            A("view.page_color", "Color"),
            A("view.page_audio", "Audio"),
            A("view.page_deliver", "Deliver"),
        }),
        M("Panels", {
            A("view.toggle_media_pool"),
            A("view.toggle_effects"),
            A("view.toggle_inspector"),
            A("view.toggle_metadata"),
            S(),
            A("view.toggle_color_timeline"),
            A("view.stacked_timelines", "Stacked Timelines"),
            A("view.toggle_edit_index"),
            A("view.toggle_keyframes"),
            S(),
            A("view.toggle_clips"),
            A("view.toggle_nodes"),
            A("view.toggle_gallery"),
            A("view.toggle_scopes"),
        }),
        M("Viewer", {
            A("view.single_viewer", "Single Viewer"),
            A("view.toggle_source_timeline", "Source / Timeline"),
            A("view.multicam_viewer", "Multicam Angles"),
            S(),
            A("view.viewer_zoom_fit", "Zoom to Fit"),
            A("view.fullscreen_viewer", "Full-Screen Viewer"),
            A("audio.float_viewer", "Float Viewer"),
            S(),
            M("On-Screen Controls", {
                A("view.onscreen_transform", "Transform"),
                A("view.onscreen_crop", "Crop"),
                A("view.onscreen_masks", "Mask"),
                A("view.onscreen_dynamic_zoom", "Dynamic Zoom"),
            }),
        }),
        // Rule 3 (two levels at most): the safe areas are part of Overlays, the guides are a
        // submenu of View.
        M("Overlays", {
            A("view.show_rulers", "Rulers"),
            A("view.grid_show", "Grid"),
            M("Grid Type", {
                A("view.grid_thirds", "Rule of Thirds"),
                A("view.grid_quarters", "4 x 4 Grid"),
                A("view.grid_custom", "Custom Grid…"),
            }),
            S(),
            A("view.safe_action", "Action Safe"),
            A("view.safe_title", "Title Safe"),
            A("view.safe_centre", "Centre Mark"),
            S(),
            M("Aspect Marker", {
                A("view.aspect_none", "None"),
                S(),
                A("view.aspect_2_39", "2.39:1"),
                A("view.aspect_1_85", "1.85:1"),
                A("view.aspect_4_3", "4:3"),
                S(),
                A("view.aspect_1_1", "1:1"),
                A("view.aspect_4_5", "4:5"),
                A("view.aspect_9_16", "9:16"),
            }),
        }),
        M("Guides", {
            A("view.guides_show", "Show"),
            A("view.guides_lock", "Lock"),
            A("view.guides_snap", "Snap"),
            S(),
            A("view.guide_add_horizontal", "Add Horizontal"),
            A("view.guide_add_vertical", "Add Vertical"),
            A("view.guides_clear", "Clear"),
            S(),
            A("view.guide_color", "Colour…"),
            S(),
            C("view.guide_presets", "Load Preset"),
            A("view.guide_save_preset", "Save Preset…"),
            C("view.guide_presets_delete", "Delete Preset"),
        }),
        S(),
        A("view.fullscreen", "Full Screen Window"),
        A("view.reset_ui_layout", "Reset Layout"),
        M("Look", {
            C("settings.color_scheme", "Colour Scheme"),
            C("settings.style", "Widget Style"),
        }),
    });
}

Menu timelineMenu()
{
    return menu("timeline", "Time&line", {
        M("Add to Timeline", {
            A("edit.insert", "Insert"),
            A("edit.overwrite", "Overwrite"),
            A("edit.replace", "Replace"),
            A("edit.place_on_top", "Place on Top"),
            S(),
            A("edit.append_at_end", "Append at End"),
            A("edit.fit_to_fill", "Fit to Fill"),
            A("edit.ripple_overwrite", "Ripple Overwrite"),
        }),
        S(),
        A("edit.split", "Split at Playhead"),
        A("edit.join", "Join Clips"),
        A("timeline.add_transition", "Add Transition"),
        A("timeline.add_subtitle", "Add Subtitle"),
        S(),
        A("timeline.snapping", "Snapping"),
        A("timeline.linked_selection", "Linked Selection"),
        S(),
        M("Tracks", {
            A("timeline.add_video_track", "Add Video Track"),
            A("timeline.add_audio_track", "Add Audio Track"),
            A("timeline.add_subtitle_track", "Add Subtitle Track"),
            S(),
            A("timeline.delete_empty_tracks", "Delete Empty Tracks"),
        }),
        M("Timelines", {
            A("timeline.new", "New Timeline…"),
            A("timeline.duplicate", "Duplicate"),
            A("timeline.delete", "Delete"),
            S(),
            A("timeline.next_tab", "Next Tab"),
            A("timeline.prev_tab", "Previous Tab"),
            A("timeline.close_tab", "Close Tab"),
            S(),
            A("timeline.settings", "Timeline Settings…"),
        }),
        M("Zoom", {
            A("timeline.zoom_in", "Zoom In"),
            A("timeline.zoom_out", "Zoom Out"),
            A("timeline.zoom_fit", "Zoom to Fit"),
        }),
    });
}

Menu clipMenu()
{
    return menu("clip", "&Clip", {
        A("clip.toggle_enabled", "Enabled"),
        A("clip.attributes", "Attributes…"),
        S(),
        M("Speed and Duration", {
            A("clip.change_speed", "Change Speed…"),
            A("clip.change_duration", "Change Duration…"),
            S(),
            A("clip.speed_increase", "Speed +10%"),
            A("clip.speed_decrease", "Speed -10%"),
            S(),
            A("clip.retime_controls", "Retime Controls"),
            A("clip.retime_curve", "Retime Curve"),
            A("clip.freeze_frame", "Freeze Frame"),
        }),
        S(),
        A("clip.link", "Link"),
        A("clip.unlink", "Unlink"),
        S(),
        M("Keyframes", {
            A("clip.add_keyframe", "Add"),
            A("clip.delete_keyframe", "Delete"),
            S(),
            A("clip.prev_keyframe", "Previous"),
            A("clip.next_keyframe", "Next"),
            S(),
            A("clip.copy_keyframes", "Copy"),
            A("clip.paste_keyframes", "Paste"),
            S(),
            M("Interpolation", {
                A("clip.keyframe_linear", "Linear"),
                A("clip.keyframe_ease_in", "Ease In"),
                A("clip.keyframe_ease_out", "Ease Out"),
                A("clip.keyframe_ease_in_out", "Ease In and Out"),
            }),
            S(),
            A("clip.toggle_keyframe_editor", "Keyframe Editor"),
            A("clip.toggle_curve_editor", "Curve Editor"),
            A("view.keyframes_panel_toggle_view", "Toggle Keyframe / Curve"),
        }),
        M("Masks", {
            A("mask.add_rectangle", "Rectangle"),
            A("mask.add_ellipse", "Ellipse"),
            A("mask.add_polygon", "Polygon"),
            A("mask.add_bezier", "Bezier"),
            A("mask.add_freehand", "Freehand"),
        }),
        M("Compound", {
            A("clip.new_compound", "New Compound Clip…"),
            A("clip.open_in_timeline", "Open in Timeline"),
            A("clip.decompose", "Decompose in Place"),
        }),
        M("Multicam", {
            A("mediapool.new_multicam", "New Multicam Clip…"),
            M("Sync and Place", {
                A("mediapool.sync_waveform", "Based on Waveform"),
                A("mediapool.sync_timecode", "Based on Timecode"),
                A("mediapool.sync_in_points", "Based on In Points"),
            }),
            A("multicam.flatten", "Flatten"),
            S(),
            A("multicam.mode_cut", "Cut Mode"),
            A("multicam.mode_switch", "Switch Mode"),
            S(),
            A("multicam.target_both", "Video and Audio"),
            A("multicam.target_video", "Video Only"),
            A("multicam.target_audio", "Audio Only"),
            S(),
            // Groups of three, like a number pad; at most five entries in a group (rule 2).
            M("Angle", {
                A("multicam.prev_page", "Previous Page"),
                A("multicam.next_page", "Next Page"),
                S(),
                A("multicam.angle_1", "Angle 1"),
                A("multicam.angle_2", "Angle 2"),
                A("multicam.angle_3", "Angle 3"),
                S(),
                A("multicam.angle_4", "Angle 4"),
                A("multicam.angle_5", "Angle 5"),
                A("multicam.angle_6", "Angle 6"),
                S(),
                A("multicam.angle_7", "Angle 7"),
                A("multicam.angle_8", "Angle 8"),
                A("multicam.angle_9", "Angle 9"),
            }),
        }),
        M("Composition", {
            A("clip.open_in_compositing", "Open in Compositing Page"),
            A("clip.reset_composition", "Reset Composition"),
            S(),
            A("clip.render_cache_output", "Cache Clip Output"),
        }),
        S(),
        A("clip.match_frame", "Match Frame"),
        A("timeline.find_in_media_pool", "Find in Media Pool"),
    });
}

Menu trimMenu()
{
    return menu("trim", "&Trim", {
        A("tool.select", "Selection Mode"),
        A("tool.trim", "Trim Mode"),
        A("tool.blade", "Blade Mode"),
        A("trim.dynamic", "Dynamic Trim"),
        S(),
        M("Trim to Playhead", {
            A("trim.trim_start", "Start"),
            A("trim.trim_end", "End"),
            S(),
            A("trim.ripple_trim_start", "Ripple Start"),
            A("trim.ripple_trim_end", "Ripple End"),
            S(),
            A("trim.extend_edit", "Extend Edit"),
        }),
        M("Nudge", {
            A("trim.nudge_left", "Left 1 Frame"),
            A("trim.nudge_right", "Right 1 Frame"),
            S(),
            A("trim.nudge_left_multi", "Left Several Frames"),
            A("trim.nudge_right_multi", "Right Several Frames"),
        }),
        M("Move", {
            A("trim.swap_left", "Swap with Previous"),
            A("trim.swap_right", "Swap with Next"),
            S(),
            A("trim.move_up", "Up a Track"),
            A("trim.move_down", "Down a Track"),
        }),
        M("Edit Point", {
            A("trim.select_nearest_edit", "Select Nearest"),
            A("trim.cycle_edit", "Cycle Side"),
        }),
    });
}

Menu markMenu()
{
    return menu("mark", "&Mark", {
        A("mark.in", "Mark In"),
        A("mark.out", "Mark Out"),
        A("mark.clip", "Mark Clip"),
        A("mark.selection", "Mark Selection"),
        S(),
        A("mark.clear_in", "Clear In"),
        A("mark.clear_out", "Clear Out"),
        A("mark.clear_both", "Clear In and Out"),
        S(),
        A("mark.add_marker", "Add Marker"),
        A("mark.modify_marker", "Add Marker with Note…"),
        A("mark.delete_marker", "Delete Marker"),
        S(),
        A("clip.flag", "Flag Clip"),
    });
}

Menu playbackMenu()
{
    return menu("playback", "&Playback", {
        A("transport.play_pause", "Play / Pause"),
        A("transport.play_reverse", "Play Reverse"),
        A("transport.shuttle_stop", "Stop"),
        M("Shuttle", {
            A("transport.shuttle_reverse", "Shuttle Reverse"),
            A("transport.shuttle_forward", "Shuttle Forward"),
        }),
        S(),
        A("transport.loop", "Loop"),
        A("transport.play_in_out", "Play In to Out"),
        A("transport.play_around", "Play Around Current Frame"),
        S(),
        M("Go To", {
            A("transport.go_to_start", "Start"),
            A("transport.go_to_end", "End"),
            S(),
            A("transport.prev_frame", "Previous Frame"),
            A("transport.next_frame", "Next Frame"),
            A("transport.back_1s", "Back 1 Second"),
            A("transport.forward_1s", "Forward 1 Second"),
            S(),
            A("transport.prev_edit", "Previous Edit"),
            A("transport.next_edit", "Next Edit"),
            S(),
            A("transport.prev_marker", "Previous Marker"),
            A("transport.next_marker", "Next Marker"),
            S(),
            A("mark.go_to_in", "In"),
            A("mark.go_to_out", "Out"),
        }),
        M("Quality", {
            A("playback.resolution_full", "Full"),
            A("playback.resolution_half", "Half"),
            A("playback.resolution_quarter", "Quarter"),
            A("playback.resolution_auto", "Automatic"),
            S(),
            A("playback.proxy_prefer", "Prefer Proxies"),
            A("playback.proxy_originals", "Prefer Originals"),
            A("playback.proxy_disable", "No Proxies"),
            S(),
            A("playback.use_proxy", "Use Proxies"),
            A("playback.use_optimized", "Use Optimized Media"),
        }),
        M("Render Cache", {
            A("playback.render_cache_none", "None"),
            A("playback.render_cache_smart", "Smart"),
            A("playback.render_cache_user", "User"),
            S(),
            A("playback.delete_render_cache", "Delete Render Cache…"),
        }),
    });
}

Menu audioMenu()
{
    return menu("audio", "&Audio", {
        A("transport.record", "Record Voice-Over…"),
        S(),
        A("clip.normalize_audio", "Normalize Levels…"),
        A("timeline.audio_scrubbing", "Audio Scrubbing"),
        S(),
        A("view.toggle_mixer", "Mixer"),
        A("audio.add_bus", "Add Bus"),
        A("audio.reset_loudness", "Reset Loudness Meter"),
    });
}

Menu helpMenu()
{
    return menu("help", "&Help", {
        A("help.guide_context", "Help for This…"),
        A("help.guide", "Guide…"),
        A("help.getting_started", "Getting Started"),
        S(),
        A("help.report_bug", "Report a Bug…"),
        A("help.community", "Discord Community"),
        S(),
        A("help.export_log", "Export Log File…"),
    });
}
// clang-format on

} // namespace

QList<Menu> streamlinedMenus()
{
    return {appMenu(), fileMenu(), editMenu(), viewMenu(), timelineMenu(), clipMenu(), trimMenu(),
        markMenu(), playbackMenu(), audioMenu(), helpMenu()};
}

// clang-format off
Menu pageMenu(const QString& pageId)
{
    if (pageId == QLatin1String("cut")) {
        return menu("page", "C&ut", {
            A("cut.smart_insert", "Smart Insert"),
            A("cut.append", "Append"),
            A("cut.close_up", "Close Up"),
            S(),
            A("cut.source_overwrite", "Source Overwrite"),
            A("cut.ripple_overwrite", "Ripple Overwrite"),
            A("cut.place_on_top", "Place on Top"),
            S(),
            A("cut.fast_review", "Fast Review"),
            S(),
            A("cut.viewer_source_clip", "Show Source Clip"),
            A("cut.viewer_source_tape", "Show Source Tape"),
            A("cut.viewer_timeline", "Show Timeline"),
        });
    }
    if (pageId == QLatin1String("compositing")) {
        return menu("page", "&Nodes", {
            A("comp.select_node", "Add Node…"),
            A("comp.add_note", "Add Note"),
            A("comp.duplicate", "Duplicate"),
            A("comp.rename", "Rename"),
            S(),
            A("comp.pass_through", "Pass Through"),
            A("comp.lock", "Lock"),
            A("comp.swap_inputs", "Swap Inputs"),
            S(),
            A("comp.view_left", "View in Left Viewer"),
            A("comp.view_right", "View in Right Viewer"),
            A("comp.fit", "Fit to View"),
            A("comp.snap_grid", "Snap to Grid"),
        });
    }
    if (pageId == QLatin1String("color")) {
        return menu("page", "&Grade", {
            M("Add Node", {
                A("color.add_serial", "Serial"),
                A("color.add_serial_before", "Serial Before"),
                A("color.add_parallel", "Parallel"),
                A("color.add_layer", "Layer"),
            }),
            A("color.delete_node", "Delete Node"),
            A("color.bypass_node", "Enable Node"),
            S(),
            A("color.copy_grade", "Copy Grade"),
            A("color.paste_grade", "Paste Grade"),
            M("Reset", {
                A("color.reset_node", "Node Grade"),
                A("color.reset_grade", "Clip Grade"),
            }),
            S(),
            A("color.bypass_all", "Bypass All Grades"),
            A("color.highlight", "Highlight"),
            A("color.timeline_grade", "Grade the Timeline"),
            S(),
            M("Stills", {
                A("color.grab_still", "Grab"),
                A("color.apply_still", "Apply"),
                A("color.wipe", "Wipe"),
            }),
            A("color.keyframe", "Add / Remove Keyframe"),
        });
    }
    return {};
}
// clang-format on

QList<HiddenAction> hiddenActions()
{
    return {
        {"timeline.target_video_*", "Track destination V1-V8: keys and track headers (rule 6)"},
        {"timeline.target_audio_*", "Track destination A1-A8: keys and track headers (rule 6)"},
        {"timeline.auto_select_video_*", "Auto select V1-V8: keys and track headers (rule 6)"},
        {"timeline.auto_select_audio_*", "Auto select A1-A8: keys and track headers (rule 6)"},
        {"view.video_scopes",
            "The floating scopes window; Scopes (Panels) is the one entry (rule 8)"},
        {"view.show_bin_list", "In the Media Pool's bin menu"},
        {"help.about_qt", "In the About box"},
        {"mediapool.new_bin", "In the Media Pool's menus"},
        {"mediapool.new_smart_bin", "In the Media Pool's menus"},
        {"mediapool.select_in_timeline", "In the Media Pool's item menu"},
        {"mediapool.save_copy", "In the Media Pool's item menu (media stored in the project)"},
        {"comp.cut", "Edit > Cut acts on the node editor while it has focus (rule 8)"},
        {"comp.copy", "Edit > Copy acts on the node editor while it has focus (rule 8)"},
        {"comp.paste", "Edit > Paste acts on the node editor while it has focus (rule 8)"},
        {"comp.delete", "Edit > Delete acts on the node editor while it has focus (rule 8)"},
        {"comp.select_all",
            "Edit > Select > All acts on the node editor while it has focus (rule 8)"},
    };
}

bool isHidden(const QString& actionId)
{
    for (const HiddenAction& hidden : hiddenActions()) {
        const QString id = QLatin1String(hidden.id);
        if (id.endsWith(QLatin1Char('*')) ? actionId.startsWith(id.chopped(1)) : actionId == id)
            return true;
    }
    return false;
}

QString layoutId(Layout layout)
{
    return QLatin1String(layout == Layout::Streamlined ? "streamlined" : "resolve");
}

Layout layoutFromId(const QString& id)
{
    return id == QLatin1String("streamlined") ? Layout::Streamlined : Layout::Resolve;
}

Layout layout()
{
    return layoutFromId(QSettings().value(QLatin1String(kSettingsKey)).toString());
}

bool streamlined()
{
    return layout() == Layout::Streamlined;
}

Notifier& notifier()
{
    static Notifier instance;
    return instance;
}

void setLayout(Layout value)
{
    if (value == layout())
        return;
    QSettings().setValue(QLatin1String(kSettingsKey), layoutId(value));
    emit notifier().changed();
}

namespace {

/// An entry that shows @p target under another name. Everything but the text follows the
/// action (checks, enabled, icon, key, tips); triggering it triggers the action.
QAction* mirror(QMenu* menu, QAction* target, const QString& id, const QString& label)
{
    auto* entry = new QAction(menu);
    entry->setProperty(kMirrorProperty, id);
    entry->setMenuRole(QAction::NoRole);
    auto sync = [entry, target, label] {
        const QString key = target->shortcut().toString(QKeySequence::NativeText);
        entry->setText(key.isEmpty() ? label : label + QLatin1Char('\t') + key);
        entry->setIcon(target->icon());
        entry->setCheckable(target->isCheckable());
        entry->setChecked(target->isChecked());
        entry->setEnabled(target->isEnabled());
        entry->setVisible(target->isVisible());
        entry->setStatusTip(target->statusTip());
        entry->setToolTip(target->toolTip());
    };
    sync();
    QObject::connect(target, &QAction::changed, entry, sync);
    QObject::connect(entry, &QAction::triggered, target, [target] { target->trigger(); });
    menu->addAction(entry);
    return entry;
}

QString slug(QString text)
{
    text = text.toLower();
    for (QChar& c : text) {
        if (!c.isLetterOrNumber())
            c = QLatin1Char('_');
    }
    return text;
}

/// The registry's own nested form (category entries, separators, submenu markers).
void fillCategory(QMenu* menu, const QString& category, ActionRegistry& registry)
{
    for (const QString& id : registry.menuEntries(category)) {
        if (id == QLatin1String(ActionRegistry::kSeparator)) {
            menu->addSeparator();
        } else if (const QString sub = ActionRegistry::submenuOf(id); !sub.isEmpty()) {
            QMenu* submenu = menu->addMenu(registry.submenuTitle(sub));
            submenu->setObjectName(QStringLiteral("menu:") + sub);
            fillCategory(submenu, sub, registry);
        } else if (registry.contains(id)) {
            menu->addAction(registry.action(id));
        }
    }
}

} // namespace

void fill(QMenu* menu, const QList<Node>& nodes, ActionRegistry& registry, const QString& path)
{
    for (const Node& node : nodes) {
        switch (node.kind) {
        case Node::Kind::Separator:
            // Never two lines in a row or a line at the edge (entries of a node may be missing).
            if (!menu->actions().isEmpty() && !menu->actions().last()->isSeparator())
                menu->addSeparator();
            break;
        case Node::Kind::Action: {
            if (!registry.contains(node.id))
                break;
            QAction* action = registry.action(node.id);
            if (node.label.isEmpty())
                menu->addAction(action);
            else
                mirror(menu, action, node.id, node.label);
            break;
        }
        case Node::Kind::Submenu: {
            QMenu* submenu = menu->addMenu(node.label);
            submenu->setObjectName(path + QLatin1Char('.') + slug(node.label));
            fill(submenu, node.children, registry, submenu->objectName());
            break;
        }
        case Node::Kind::Category: {
            QMenu* submenu = menu->addMenu(node.label);
            submenu->setObjectName(QStringLiteral("menu:") + node.id);
            fillCategory(submenu, node.id, registry);
            break;
        }
        }
    }
    while (!menu->actions().isEmpty() && menu->actions().last()->isSeparator())
        delete menu->actions().last();
}

bool paletteBreakBefore(int index)
{
    constexpr int kPaletteGroup = 4;
    return index > 0 && index % kPaletteGroup == 0 && streamlined();
}

QAction* addCommand(QMenu* menu, ActionRegistry& registry, const QString& id, const QString& label)
{
    if (!registry.contains(id))
        return nullptr;
    QAction* action = registry.action(id);
    if (label.isEmpty()) {
        menu->addAction(action);
        return action;
    }
    return mirror(menu, action, id, label);
}

QString mirroredId(const QAction* entry)
{
    return entry ? entry->property(kMirrorProperty).toString() : QString();
}

} // namespace kk::menulayout
