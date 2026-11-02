#!/usr/bin/env python3
# Source of truth for the built-in "resolve-windows" keymap profile: generates
# resources/keymaps/resolve-windows.json and docs/keymap-resolve-windows.md from one table, so
# the shortcuts and their documented confidence never drift apart.
# Usage: scripts/keymap-resolve-windows.py   (from anywhere; writes into the repo)
import json, sys, collections
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
S, L, U = "sure", "likely", "unsure"
# (section, id, command text, keys, confidence, note)
rows = []
def sec(name): rows.append((name,))
def a(id, text, keys, conf, note=""):
    rows.append((id, text, keys, conf, note))

sec("File and project")
a("file.new", "New Project", [], U, "no known default (projects are created in Resolve's Project Manager); Ctrl+N is New Timeline")
a("timeline.new", "New Timeline", ["Ctrl+N"], L, "File > New Timeline in Resolve; the dialog asks for the name")
a("file.open", "Open Project / Project Manager", ["Ctrl+O", "Shift+1"], L, "Shift+1 is Resolve's Project Manager page; Kinoklip has no project manager yet and opens a project instead. Ctrl+O unsure.")
a("file.save", "Save Project", ["Ctrl+S"], S)
a("file.save_as", "Save Project As", ["Ctrl+Shift+S"], S)
a("file.import_media", "Import Media", ["Ctrl+I"], S)
a("file.import_subtitle", "Import Subtitle", [], L, "File > Import > Subtitle in Resolve; no default key known")
a("file.project_settings", "Project Settings", ["Shift+9"], S)
a("file.quit", "Quit", ["Ctrl+Q"], S)

sec("Edit")
a("edit.undo", "Undo", ["Ctrl+Z"], S)
a("edit.redo", "Redo", ["Ctrl+Shift+Z"], S)
a("edit.cut", "Cut", ["Ctrl+X"], S)
a("edit.copy", "Copy", ["Ctrl+C"], S)
a("edit.paste", "Paste", ["Ctrl+V"], S)
a("edit.paste_insert", "Paste Insert", ["Ctrl+Shift+V"], S)
a("edit.paste_attributes", "Paste Attributes", ["Alt+V"], S)
a("edit.ripple_cut", "Ripple Cut", ["Ctrl+Shift+X"], S)
a("edit.delete", "Delete Selected (lift, leaves a gap)", ["Backspace"], S)
a("edit.ripple_delete", "Ripple Delete", ["Del", "Shift+Backspace"], S)
a("edit.delete_gaps", "Delete Gaps", [], L, "menu command without a default key")
a("edit.select_all", "Select All", ["Ctrl+A"], S)
a("edit.deselect_all", "Deselect All", ["Ctrl+Shift+A"], S)
a("edit.select_forward", "Select Clips Forward (auto-select tracks)", ["Y"], U)
a("edit.select_backward", "Select Clips Backward (auto-select tracks)", ["Ctrl+Y"], U)
a("edit.select_forward_all", "Select Clips Forward on All Tracks", ["Alt+Y"], U)
a("edit.select_backward_all", "Select Clips Backward on All Tracks", ["Ctrl+Alt+Y"], U)
a("edit.insert", "Insert Clip", ["F9"], S)
a("edit.overwrite", "Overwrite Clip", ["F10"], S)
a("edit.ripple_overwrite", "Ripple Overwrite", ["Shift+F10"], L)
a("edit.replace", "Replace Clip", ["F11"], S)
a("edit.fit_to_fill", "Fit to Fill", ["Shift+F11"], S)
a("edit.place_on_top", "Place on Top", ["F12"], S)
a("edit.append_at_end", "Append at End", ["Shift+F12"], S)
a("edit.keyboard_customization", "Keyboard Customization", ["Ctrl+Alt+K"], S)
a("edit.preferences", "Preferences", ["Ctrl+,"], L)

sec("Tools and trimming")
a("tool.select", "Selection Mode", ["A"], S)
a("tool.trim", "Trim Edit Mode", ["T"], S)
a("tool.blade", "Blade Edit Mode", ["B"], S)
a("trim.dynamic", "Dynamic Trim Mode (J/K/L trim the selected edit)", ["W"], S)
a("trim.nudge_left", "Nudge / Trim One Frame Left", [","], S)
a("trim.nudge_right", "Nudge / Trim One Frame Right", ["."], S)
a("trim.nudge_left_multi", "Nudge / Trim Several Frames Left", ["Shift+,"], S)
a("trim.nudge_right_multi", "Nudge / Trim Several Frames Right", ["Shift+."], S)
a("trim.trim_start", "Trim Start to Playhead (leaves a gap)", ["Shift+["], S)
a("trim.trim_end", "Trim End to Playhead (leaves a gap)", ["Shift+]"], S)
a("trim.ripple_trim_start", "Ripple Trim Start to Playhead (closes the gap)", ["Ctrl+Shift+["], S, "On a US layout Qt may deliver Ctrl+Shift+{; the timeline accepts both (spec 8.10).")
a("trim.ripple_trim_end", "Ripple Trim End to Playhead (closes the gap)", ["Ctrl+Shift+]"], S, "On a US layout Qt may deliver Ctrl+Shift+}; the timeline accepts both (spec 8.10).")
a("trim.extend_edit", "Extend Edit to Playhead", ["E"], S)
a("trim.cycle_edit", "Toggle Edit Point Side (out / roll / in)", ["U"], S)
a("trim.select_nearest_edit", "Select Nearest Edit Point", ["V"], S)
a("trim.select_nearest_clip", "Select Nearest Clip or Gap", ["Shift+V"], L)
a("trim.swap_left", "Swap Clip with Previous", ["Ctrl+Shift+,"], L)
a("trim.swap_right", "Swap Clip with Next", ["Ctrl+Shift+."], L)

sec("Timeline")
a("edit.split", "Split Clip (Razor) at Playhead", ["Ctrl+\\"], S, "Ctrl+B is not a default (owner feedback alpha.1 #8).")
a("edit.join", "Join Clips (remove a through edit)", ["Alt+\\"], U)
a("timeline.add_transition", "Add Default Transition", ["Ctrl+T"], S)
a("timeline.snapping", "Snapping", ["N"], S)
a("timeline.linked_selection", "Linked Selection", ["Ctrl+Shift+L"], S)
a("timeline.audio_scrubbing", "Audio Scrubbing (disabled: no audio scrubbing yet)", ["Shift+S"], S)
a("timeline.zoom_in", "Zoom In", ["Ctrl+="], S)
a("timeline.zoom_out", "Zoom Out", ["Ctrl+-"], S)
a("timeline.zoom_fit", "Zoom to Fit", ["Shift+Z"], S)
a("timeline.add_video_track", "Add Video Track", [], L)
a("timeline.add_audio_track", "Add Audio Track", [], L)
a("timeline.add_subtitle_track", "Add Subtitle Track", [], L, "Timeline > Add Track > Subtitle Track in Resolve; no default key known")
a("timeline.add_subtitle", "Add Subtitle (caption at the playhead)", [], U, "Resolve adds captions from the subtitle track's right-click menu; no default key known")
a("timeline.duplicate", "Duplicate Timeline", [], L, "media pool / Timeline menu command without a default key")
a("timeline.delete", "Delete Timeline", [], L, "media pool command without a default key (Delete in the media pool deletes the selected timeline item)")
a("timeline.settings", "Timeline Settings", [], U, "no known default key")
a("timeline.next_tab", "Next Timeline Tab", [], U, "no known Resolve default; click the tab or use the timeline drop-down")
a("timeline.prev_tab", "Previous Timeline Tab", [], U, "no known Resolve default")
a("timeline.close_tab", "Close Timeline Tab", [], U, "no known Resolve default; the tab's close button or its context menu")

sec("Track destination and auto track selectors")
for n in range(1, 9):
    a(f"timeline.target_video_{n}", f"Video Destination V{n}", [f"Alt+{n}"], L)
for n in range(1, 9):
    a(f"timeline.target_audio_{n}", f"Audio Destination A{n}", [f"Ctrl+Alt+{n}"], U)
for n in range(1, 9):
    a(f"timeline.auto_select_video_{n}", f"Toggle Auto Select V{n}", [f"Ctrl+{n}"], L)
for n in range(1, 9):
    a(f"timeline.auto_select_audio_{n}", f"Toggle Auto Select A{n}", [f"Ctrl+Shift+{n}"], U)

sec("Clip")
a("clip.toggle_enabled", "Enable / Disable Clip", ["D"], S)
a("clip.match_frame", "Match Frame (load the source at this frame)", ["F"], S)
a("clip.change_duration", "Change Clip Duration", ["Ctrl+D"], S)
a("clip.change_speed", "Change Clip Speed", ["R"], L)
a("clip.retime_controls", "Retime Controls (speed bar on the selected clips)", ["Ctrl+R"], S)
a("clip.freeze_frame", "Freeze Frame (the clip shows the frame at the playhead)", ["Shift+R"], S)
a("clip.retime_curve", "Retime Curve (opens the clip's retime curve in the curve editor)", ["Ctrl+Shift+R"], L)
a("clip.new_compound", "New Compound Clip", [], L, "Clip menu / clip context menu command without a default key in Resolve")
a("clip.open_in_timeline", "Open in Timeline (compound clip)", [], L, "context menu command without a default key")
a("clip.decompose", "Decompose in Place (compound clip)", [], L, "context menu command without a default key")
a("clip.link", "Link / Unlink Clips", ["Ctrl+Alt+L"], L, "Resolve has one toggle; Kinoklip's Link toggles too.")
a("clip.unlink", "Unlink Clips", [], L)
a("clip.add_keyframe", "Add Keyframe (inspector)", ["Alt+K"], U, "toggles a key on the last touched inspector parameter; Resolve default not confirmed")
a("mask.add_rectangle", "Add Rectangle Mask", [], U, "Clip > Add Mask; no Resolve default key known, none assigned. In the viewer, editing keys (Delete, Backspace, Enter, Esc) act on the active mask while the viewer has focus.")
a("mask.add_ellipse", "Add Ellipse Mask", [], U, "no default key known")
a("mask.add_polygon", "Add Polygon Mask (click points in the viewer)", [], U, "no default key known")
a("mask.add_bezier", "Add Bezier Mask (pen: click corners, drag smooth points)", [], U, "no default key known")
a("mask.add_freehand", "Add Freehand Mask (draw in the viewer)", [], U, "no default key known")
a("clip.prev_keyframe", "Previous Keyframe", ["["], L)
a("clip.next_keyframe", "Next Keyframe", ["]"], L)
a("clip.delete_keyframe", "Delete Keyframes", [], U, "no known default; Delete / Backspace act on selected keys while the keyframe or curve editor has focus")
a("clip.copy_keyframes", "Copy Keyframes", [], U, "no known default; Ctrl+C acts on selected keys while the keyframe or curve editor has focus")
a("clip.paste_keyframes", "Paste Keyframes", [], U, "no known default; Ctrl+V pastes keys at the playhead while the keyframe or curve editor has focus")
a("clip.toggle_keyframe_editor", "Show Keyframe Editor", [], L, "Resolve uses the clip's keyframe button; no default key")
a("clip.toggle_curve_editor", "Show Curve Editor", [], L, "Resolve uses the clip's curve button; no default key")

sec("Marking and markers")
a("mark.in", "Mark In", ["I"], S)
a("mark.out", "Mark Out", ["O"], S)
a("mark.clip", "Mark Clip", ["X"], S)
a("mark.selection", "Mark Selection", ["Shift+A"], S)
a("mark.clear_in", "Clear In", ["Alt+I"], S)
a("mark.clear_out", "Clear Out", ["Alt+O"], S)
a("mark.clear_both", "Clear In and Out", ["Alt+X"], S)
a("mark.go_to_in", "Go to In", ["Shift+I"], S)
a("mark.go_to_out", "Go to Out", ["Shift+O"], S)
a("mark.add_marker", "Add Marker (on the selected clip: clip marker)", ["M"], S)
a("mark.modify_marker", "Add and Modify Marker", ["Ctrl+M"], S)
a("mark.delete_marker", "Delete Marker", ["Alt+M"], L)

sec("Pages and view")
a("view.page_media", "Media Page", ["Shift+2"], S)
a("view.page_cut", "Cut Page (disabled)", ["Shift+3"], S)
a("view.page_edit", "Edit Page", ["Shift+4"], S)
a("view.page_compositing", "Compositing Page (disabled; Resolve: Fusion)", ["Shift+5"], S)
a("view.page_color", "Color Page (disabled)", ["Shift+6"], S)
a("view.page_audio", "Audio Page (disabled; Resolve: Fairlight)", ["Shift+7"], S)
a("view.page_deliver", "Deliver Page", ["Shift+8"], S)
a("view.toggle_media_pool", "Media Pool panel", [], L)
a("view.toggle_effects", "Effects panel", [], L)
a("view.toggle_edit_index", "Edit Index panel", [], L)
a("view.toggle_keyframes", "Keyframes panel (keyframe editor window)", [], U, "Kinoklip's large keyframe editor; no default key is known, none assigned.")
a("view.toggle_mixer", "Mixer panel", [], L)
a("view.toggle_metadata", "Metadata panel", [], L)
a("view.toggle_inspector", "Inspector panel", [], L)
a("view.single_viewer", "Single Viewer Mode", [], U)
a("view.toggle_source_timeline", "Toggle Source / Timeline Viewer", ["Q"], S)
a("view.fullscreen_viewer", "Viewer Full Screen", ["Ctrl+F"], L)
a("view.onscreen_masks", "On-Screen Mask Controls (viewer on-screen controls: Mask)", [], U, "Resolve picks the viewer overlay from the on-screen controls menu; no default key is known, none assigned.")
a("view.fullscreen", "Full Screen Window", ["Ctrl+Shift+F"], U)

sec("Playback and navigation")
a("transport.play_pause", "Play / Stop", ["Space"], S)
a("transport.play_reverse", "Play Reverse", ["Shift+Space"], L)
a("transport.shuttle_reverse", "Shuttle Reverse (J, again = faster)", ["J"], S)
a("transport.shuttle_stop", "Stop (K; K+J / K+L step and slow play)", ["K"], S)
a("transport.shuttle_forward", "Shuttle Forward (L, again = faster)", ["L"], S)
a("transport.prev_frame", "Previous Frame", ["Left"], S)
a("transport.next_frame", "Next Frame", ["Right"], S)
a("transport.back_1s", "Back One Second", ["Shift+Left"], S)
a("transport.forward_1s", "Forward One Second", ["Shift+Right"], S)
a("transport.prev_edit", "Previous Edit", ["Up"], S)
a("transport.next_edit", "Next Edit", ["Down"], S)
a("transport.go_to_start", "Go to Timeline Start", ["Home"], S)
a("transport.go_to_end", "Go to Timeline End", ["End"], S)
a("transport.prev_marker", "Previous Marker", ["Shift+Up"], S)
a("transport.next_marker", "Next Marker", ["Shift+Down"], S)
a("transport.loop", "Loop", ["Ctrl+/"], S)
a("transport.play_in_out", "Play In to Out", ["Alt+/"], S)
a("transport.play_around", "Play Around Current Frame", ["/"], S)
a("playback.resolution_full", "Timeline Proxy Resolution: Full", [], S, "menu command without a default key")
a("playback.resolution_half", "Timeline Proxy Resolution: Half", [], S, "menu command without a default key")
a("playback.resolution_quarter", "Timeline Proxy Resolution: Quarter", [], S, "menu command without a default key")
a("playback.resolution_auto", "Automatic proxy resolution (Kinoklip)", [], S, "no Resolve equivalent; Resolve's closest is Performance Mode")
a("playback.proxy_prefer", "Proxy Handling: Prefer Proxies", [], L, "menu command without a default key (Playback > Proxy Handling)")
a("playback.proxy_originals", "Proxy Handling: Prefer Camera Originals", [], L, "menu command without a default key")
a("playback.proxy_disable", "Proxy Handling: Disable All Proxies", [], L, "menu command without a default key")
a("playback.use_proxy", "Use Proxy Media If Available", [], U, "no default key known; toggles between Prefer Proxies and Disable All Proxies")
a("playback.use_optimized", "Use Optimized Media If Available", [], L, "menu command without a default key")
a("playback.render_cache_none", "Render Cache: None", [], L, "menu command without a default key")
a("playback.render_cache_smart", "Render Cache: Smart", [], L, "menu command without a default key")
a("playback.render_cache_user", "Render Cache: User", [], L, "menu command without a default key")
a("playback.delete_render_cache", "Delete Render Cache (all)", [], U, "Resolve offers All / Unused / Selected Clips; Kinoklip deletes all")
a("clip.render_cache_output", "Render Cache Clip Output", [], L, "clip context menu command without a default key")

sec("Media pool")
a("mediapool.generate_optimized", "Generate Optimized Media", [], L, "media pool context menu, no default key")
a("mediapool.generate_proxy", "Generate Proxy Media", [], L, "media pool context menu, no default key")
a("mediapool.stop_generation", "Stop generating proxy / optimized media", [], U, "Resolve stops from its progress dialog; Kinoklip from the context menu")
a("mediapool.link_proxy", "Link Proxy Media", [], L, "media pool context menu, no default key")
a("mediapool.unlink_proxy", "Unlink Proxy Media", [], L, "media pool context menu, no default key")
a("mediapool.delete_proxy", "Delete Proxy Media", [], L, "media pool context menu, no default key")
a("mediapool.delete_optimized", "Delete Optimized Media", [], L, "media pool context menu, no default key")

sec("Help")
a("help.about", "About", [], S)
a("help.about_qt", "About Qt", [], S)

shortcuts = collections.OrderedDict()
for r in rows:
    if len(r) > 1:
        shortcuts[r[0]] = r[2]
data = collections.OrderedDict()
data["formatVersion"] = 1
data["name"] = "DaVinci Resolve (Windows)"
data["comment"] = ("Built-in default profile: Resolve's Windows default shortcuts, documented with a "
    "confidence per key in docs/keymap-resolve-windows.md. Keys use QKeySequence portable text. "
    "Read-only: users rebind in Keyboard Customization; overrides live in the user settings or in a user profile.")
data["shortcuts"] = shortcuts
lines = ["{", '    "formatVersion": 1,', '    "name": ' + json.dumps(data["name"]) + ",",
    '    "comment": ' + json.dumps(data["comment"]) + ",", '    "shortcuts": {']
items = list(shortcuts.items())
for i, (k, v) in enumerate(items):
    lines.append("        " + json.dumps(k) + ": " + json.dumps(v, ensure_ascii=False) + ("," if i + 1 < len(items) else ""))
lines += ["    }", "}"]
open(f"{root}/resources/keymaps/resolve-windows.json", "w").write("\n".join(lines) + "\n")

def md(k): return " / ".join("`" + x.replace("|","\\|") + "`" for x in k) if k else "(none)"
counts = collections.Counter(r[3] for r in rows if len(r) > 1)
out = ["# Keymap: DaVinci Resolve (Windows)", "",
"The built-in default keyboard profile (`resources/keymaps/resolve-windows.json`, profile id",
"`resolve-windows`). It reproduces the default Windows shortcuts of DaVinci Resolve's Edit page and",
"its global commands, written from general knowledge of how Resolve behaves (nothing copied from",
"Resolve's documentation). Commands of subsystems Kinoklip does not have yet (Cut, Compositing, Color and",
"Audio pages, audio scrubbing) keep their Resolve key but are",
"disabled.", "",
"**Confidence** says how sure we are that the key is Resolve's default:",
"- **sure**: well-known default, used daily;",
"- **likely**: believed correct, not verified against a running Resolve;",
"- **unsure**: best guess; please correct in the keymap file (and here) if it differs.", "",
f"Counts: {sum(counts.values())} commands; sure {counts[S]}, likely {counts[L]}, unsure {counts[U]}.", "",
"Mouse (EditBehaviour, Preferences > Editing): wheel scrolls the tracks vertically, Ctrl+wheel",
"scrolls the timeline horizontally, Alt+wheel zooms the timeline (around the playhead by default;",
"\"Zoom around: Mouse cursor\" anchors it at the pointer), Shift+wheel changes the track height.",
"Confidence: likely.", "",
"To change a default: edit the table in `scripts/keymap-resolve-windows.py` and run it (it writes",
"the JSON file and this page); `test_actionregistry` checks",
"that every command has an entry and that no key is used twice.", ""]
for r in rows:
    if len(r) == 1:
        out += ["", f"## {r[0]}", "", "| Command | Action id | Key | Confidence | Note |", "|---|---|---|---|---|"]
    else:
        id, text, keys, conf, note = r
        out.append(f"| {text} | `{id}` | {md(keys)} | {conf} | {note} |")
open(f"{root}/docs/keymap-resolve-windows.md", "w").write("\n".join(out) + "\n")
print(counts)
