#!/usr/bin/env python3
# Source of truth for the built-in keymap presets that follow other video editors: generates
# resources/keymaps/<id>.json and docs/keymaps/<id>.md for each editor below, so the keys and
# their documented confidence never drift apart.
#
# Every preset starts from the commands of the default profile (scripts/keymap-resolve-windows.py,
# whose table is read, never changed): a command with a counterpart in the other editor gets that
# editor's key (a row below, with a confidence); every other command keeps its Resolve key, or is
# left unbound when the other editor uses that key for something else. The key assignments come
# from general knowledge of how those editors are used; nothing is copied from their manuals.
#
# Usage: scripts/keymap-presets.py   (from anywhere; writes into the repo; exits non-zero when a
# preset binds one key twice in one scope)
import collections
import json
import os
import re
import sys

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
S, L, U = "sure", "likely", "unsure"

# ---- The default profile's table (read from its generator, which stays untouched) ----

def resolve_rows():
    path = os.path.join(root, "scripts", "keymap-resolve-windows.py")
    src = open(path, encoding="utf-8").read()
    marker = "\nshortcuts = collections.OrderedDict()"
    if marker not in src:
        sys.exit("keymap-resolve-windows.py changed shape: table end marker not found")
    ns = {"__file__": path}
    exec(compile(src[: src.index(marker)], path, "exec"), ns)
    return ns["rows"]

ROWS = resolve_rows()
ACTIONS = collections.OrderedDict((r[0], r) for r in ROWS if len(r) > 1)  # id -> row
SECTION = {}
_sec = ""
for r in ROWS:
    if len(r) == 1:
        _sec = r[0]
    else:
        SECTION[r[0]] = _sec

def scope(action_id):
    # Mirrors StandardActions.cpp: the Cut page's commands act only on that page, the node
    # editor's and the Color page's only while they have focus.
    prefix = action_id.split(".")[0]
    return {"cut": "page:cut", "comp": "widget:node_editor", "color": "widget:color_page"}.get(prefix, "")

def scopes_conflict(a, b):
    # ActionRegistry::scopesConflict()
    if a == b:
        return True
    return (a == "" and b.startswith("page:")) or (b == "" and a.startswith("page:"))

KEY_RE = re.compile(r"^((Ctrl|Shift|Alt|Meta|Num)\+)*([A-Z0-9]|F[0-9]{1,2}|Space|Left|Right|Up|Down|Home|End|PgUp|PgDown|Del|Backspace|Return|Tab|Esc|[\[\]\\/,.;'`=\-])$")

# ---- Presets ----

class Preset:
    def __init__(self, pid, name, editor, intro, gaps):
        self.id, self.name, self.editor, self.intro, self.gaps = pid, name, editor, intro, gaps
        self.rows = collections.OrderedDict()  # id -> (keys, confidence, note)

    def m(self, action_id, keys, conf, note=""):
        if action_id not in ACTIONS:
            sys.exit(f"{self.id}: unknown action {action_id}")
        if action_id in self.rows:
            sys.exit(f"{self.id}: {action_id} mapped twice")
        for k in keys:
            if not KEY_RE.match(k):
                sys.exit(f"{self.id}: {action_id}: bad key text {k!r}")
        self.rows[action_id] = (list(keys), conf, note)

PRESETS = []
def preset(*args):
    p = Preset(*args)
    PRESETS.append(p)
    return p

# Commands that are the same in almost every editor on Windows and Linux.
def common(p, quit=True, select_all=True):
    p.m("transport.play_pause", ["Space"], S)
    p.m("edit.undo", ["Ctrl+Z"], S)
    p.m("edit.cut", ["Ctrl+X"], S)
    p.m("edit.copy", ["Ctrl+C"], S)
    p.m("edit.paste", ["Ctrl+V"], S)
    p.m("file.open", ["Ctrl+O"], S)
    p.m("file.save", ["Ctrl+S"], S)
    p.m("transport.go_to_start", ["Home"], S)
    p.m("transport.go_to_end", ["End"], S)
    p.m("transport.prev_frame", ["Left"], S)
    p.m("transport.next_frame", ["Right"], S)
    if select_all:
        p.m("edit.select_all", ["Ctrl+A"], L)
    if quit:
        p.m("file.quit", ["Ctrl+Q"], L)

# ---- Vegas Pro (Windows) ----
p = preset("vegas-pro", "Vegas Pro", "Vegas Pro",
    ["Vegas Pro's default Windows keys. **S splits the clip at the playhead**, the reason this",
     "preset exists. Vegas has no three-point editing or page layout, so insert / overwrite and",
     "the page keys keep Kinoklip's (Resolve's) keys."],
    ["Auto Ripple on / off (Ctrl+L) and post-edit ripple (F): Kinoklip has Ripple Delete instead.",
     "Region markers (R), loop region editing, event envelopes, crossfade options.",
     "Edit tool cycling (D) and the Envelope / Selection edit tools.",
     "Track mute / solo keys and the Explorer, Trimmer and Mixer window keys (Alt+1 ... Alt+9 style).",
     "Render As (Ctrl+M): use the Deliver page (Shift+8)."])
common(p)
p.m("edit.split", ["S"], S, "splits the selected clips at the playhead, or every clip there when none is selected")
p.m("transport.shuttle_reverse", ["J"], L)
p.m("transport.shuttle_stop", ["K"], L)
p.m("transport.shuttle_forward", ["L"], L)
p.m("transport.loop", ["Q"], S, "Vegas toggles loop playback with Q")
p.m("mark.in", ["I"], L, "Vegas sets the start of the loop region (selection) with I")
p.m("mark.out", ["O"], L, "and its end with O")
p.m("mark.add_marker", ["M"], S)
p.m("transport.prev_marker", ["Ctrl+Left"], L, "previous marker or region edge in Vegas")
p.m("transport.next_marker", ["Ctrl+Right"], L)
p.m("edit.redo", ["Ctrl+Y", "Ctrl+Shift+Z"], L, "Ctrl+Y in Vegas; Ctrl+Shift+Z kept as a second key")
p.m("edit.delete", ["Del"], S, "deletes the selected events, leaving a gap (unless Auto Ripple is on in Vegas)")
p.m("edit.ripple_delete", ["Shift+Backspace"], U, "no direct counterpart (Vegas ripples through Auto Ripple, Ctrl+L); Resolve's second key kept")
p.m("timeline.snapping", ["F8"], S, "Enable Snapping in Vegas")
p.m("timeline.zoom_in", ["Up"], L, "Up / Down zoom the timeline in Vegas")
p.m("timeline.zoom_out", ["Down"], L)
p.m("clip.link", ["G"], L, "Group in Vegas")
p.m("clip.unlink", ["U"], L, "Ungroup in Vegas")
p.m("timeline.linked_selection", ["Ctrl+Shift+U"], L, "Ignore Event Grouping in Vegas")
p.m("trim.nudge_left", ["Num+4", ","], L, "keypad 4 / 6 move the selected events by a frame in Vegas; Resolve's , / . kept")
p.m("trim.nudge_right", ["Num+6", "."], L)
p.m("trim.trim_start", ["Alt+["], L, "trim the event start to the cursor")
p.m("trim.trim_end", ["Alt+]"], L, "trim the event end to the cursor")
p.m("file.new", ["Ctrl+N"], S, "a new project")
p.m("file.save_as", ["Ctrl+Shift+S"], L)
p.m("view.fullscreen", ["F11"], U, "full-screen window; not a known Vegas key")

# ---- Premiere Pro (Windows) ----
p = preset("premiere-pro", "Premiere Pro (Windows)", "Premiere Pro",
    ["Premiere Pro's default Windows keys. Premiere's panel keys (Shift+1 ... Shift+9) show",
     "Kinoklip's matching panels; its workspaces (Alt+Shift+1 ...) switch Kinoklip's pages."],
    ["Slip (Y), Slide (U), Rate Stretch (R), Rolling Edit (N), Pen (P), Hand (H) and Zoom (Z) tools: Kinoklip's Trim tool (B here) does ripple, roll, slip and slide by where you drag.",
     "Add Edit to All Tracks (Ctrl+Shift+K), Lift (;) and Extract (') of the In / Out range, Nest, Render In to Out (Enter).",
     "Link / Unlink is one toggle in Premiere (Ctrl+L): here Ctrl+L links (and unlinks a linked group); Unlink has no key.",
     "Speed / Duration is one dialog in Premiere (Ctrl+R): here Ctrl+R changes the speed; Change Duration has no key.",
     "Shift+Left / Right step 5 frames in Premiere, 1 second in Kinoklip."])
common(p)
p.m("edit.redo", ["Ctrl+Shift+Z"], S)
p.m("transport.shuttle_reverse", ["J"], S)
p.m("transport.shuttle_stop", ["K"], S)
p.m("transport.shuttle_forward", ["L"], S)
p.m("transport.back_1s", ["Shift+Left"], L, "5 frames in Premiere")
p.m("transport.forward_1s", ["Shift+Right"], L, "5 frames in Premiere")
p.m("transport.prev_edit", ["Up", "PgUp"], S)
p.m("transport.next_edit", ["Down", "PgDown"], S)
p.m("transport.play_in_out", ["Ctrl+Shift+Space"], L)
p.m("transport.play_around", ["Shift+K"], L)
p.m("mark.in", ["I"], S)
p.m("mark.out", ["O"], S)
p.m("mark.clip", ["X"], S)
p.m("mark.selection", ["/"], S)
p.m("mark.clear_in", ["Ctrl+Shift+I"], S)
p.m("mark.clear_out", ["Ctrl+Shift+O"], S)
p.m("mark.clear_both", ["Ctrl+Shift+X"], S)
p.m("mark.go_to_in", ["Shift+I"], S)
p.m("mark.go_to_out", ["Shift+O"], S)
p.m("mark.add_marker", ["M"], S)
p.m("transport.next_marker", ["Shift+M"], S)
p.m("transport.prev_marker", ["Ctrl+Shift+M"], S)
p.m("edit.insert", [","], S)
p.m("edit.overwrite", ["."], S)
p.m("edit.split", ["Ctrl+K"], S, "Add Edit")
p.m("edit.delete", ["Del", "Backspace"], S, "Clear")
p.m("edit.ripple_delete", ["Shift+Del"], L)
p.m("edit.paste_insert", ["Ctrl+Shift+V"], S)
p.m("edit.paste_attributes", ["Ctrl+Alt+V"], S)
p.m("edit.deselect_all", ["Ctrl+Shift+A"], S)
p.m("edit.select_forward", ["A"], L, "Track Select Forward tool")
p.m("edit.select_backward", ["Shift+A"], L, "Track Select Backward tool")
p.m("tool.select", ["V"], S)
p.m("tool.blade", ["C"], S, "Razor tool")
p.m("tool.trim", ["B"], L, "Ripple Edit tool; Kinoklip's Trim tool also rolls, slips and slides")
p.m("trim.select_nearest_edit", ["T"], L, "Select Nearest Edit Point")
p.m("trim.extend_edit", ["E"], S)
p.m("trim.select_nearest_clip", ["D"], L, "Select Clip at Playhead")
p.m("trim.ripple_trim_start", ["Q"], S, "Ripple Trim Previous Edit to Playhead")
p.m("trim.ripple_trim_end", ["W"], S, "Ripple Trim Next Edit to Playhead")
p.m("trim.trim_start", ["Ctrl+Alt+Q"], L, "Trim Previous Edit to Playhead")
p.m("trim.trim_end", ["Ctrl+Alt+W"], L, "Trim Next Edit to Playhead")
p.m("trim.nudge_left", ["Alt+Left"], S, "Nudge Clip Selection Left (one frame)")
p.m("trim.nudge_right", ["Alt+Right"], S)
p.m("trim.nudge_left_multi", ["Alt+Shift+Left"], L, "five frames")
p.m("trim.nudge_right_multi", ["Alt+Shift+Right"], L)
p.m("timeline.snapping", ["S"], S)
p.m("timeline.audio_scrubbing", ["Shift+S"], U, "audio while scrubbing; not sure Premiere has a default key")
p.m("timeline.zoom_in", ["="], S)
p.m("timeline.zoom_out", ["-"], S)
p.m("timeline.zoom_fit", ["\\"], S, "Zoom to Sequence")
p.m("timeline.add_transition", ["Ctrl+D"], S, "Apply Video Transition")
p.m("timeline.new", ["Ctrl+N"], S, "New Sequence")
p.m("clip.toggle_enabled", ["Shift+E"], S)
p.m("clip.match_frame", ["F"], S)
p.m("clip.change_speed", ["Ctrl+R"], S, "Speed / Duration")
p.m("clip.link", ["Ctrl+L"], S, "Link / Unlink toggle")
p.m("file.new", ["Ctrl+Alt+N"], S, "New Project")
p.m("file.save_as", ["Ctrl+Shift+S"], S)
p.m("file.import_media", ["Ctrl+I"], S)
p.m("mediapool.new_bin", ["Ctrl+B"], S)
p.m("edit.keyboard_customization", ["Ctrl+Alt+K"], S)
p.m("view.page_deliver", ["Ctrl+M"], L, "Export Media opens Premiere's export; Kinoklip's is the Deliver page")
p.m("view.fullscreen_viewer", ["Ctrl+`"], L, "Toggle Full Screen")
p.m("view.toggle_media_pool", ["Shift+1"], L, "Project panel")
p.m("view.toggle_inspector", ["Shift+5"], L, "Effect Controls panel")
p.m("view.toggle_mixer", ["Shift+6"], L, "Audio Track Mixer")
p.m("view.toggle_effects", ["Shift+7"], L, "Effects panel")
for n, page in enumerate(["media", "cut", "edit", "compositing", "color", "audio"], start=1):
    p.m(f"view.page_{page}", [f"Alt+Shift+{n}"], U, "Premiere's workspaces 1-6 map to Kinoklip's pages" if n == 1 else "")

# ---- Final Cut Pro (Command becomes Ctrl) ----
p = preset("final-cut-pro", "Final Cut Pro (Ctrl for Command)", "Final Cut Pro",
    ["Final Cut Pro's default keys, moved to a PC keyboard: **Command becomes Ctrl**, Option",
     "becomes Alt, and Control becomes Meta (the Super / Windows key, which some desktops keep for",
     "themselves: rebind those few in Keyboard Customization if they do nothing). Final Cut's",
     "magnetic timeline closes gaps by itself, so Delete is Ripple Delete here and Shift+Delete",
     "leaves a gap."],
    ["Connect (Q) puts the clip on the track above (Place on Top); there are no connected clips or storylines (G).",
     "Position (P) and Range (R) tools, Skimming (S), Solo (Alt+S), Audition (Y).",
     "Lift from Storyline (Ctrl+Alt+Up), Overwrite to Primary (Ctrl+Alt+Down).",
     "Final Cut saves by itself; Ctrl+S saves here as usual.",
     "Shift+Left / Right step 10 frames in Final Cut, 1 second in Kinoklip."])
common(p, select_all=False)
p.m("edit.select_all", ["Ctrl+A"], S)
p.m("edit.redo", ["Ctrl+Shift+Z"], S)
p.m("transport.shuttle_reverse", ["J"], S)
p.m("transport.shuttle_stop", ["K"], S)
p.m("transport.shuttle_forward", ["L"], S)
p.m("transport.back_1s", ["Shift+Left"], L, "10 frames in Final Cut")
p.m("transport.forward_1s", ["Shift+Right"], L, "10 frames in Final Cut")
p.m("transport.prev_edit", ["Up"], S)
p.m("transport.next_edit", ["Down"], S)
p.m("transport.loop", ["Ctrl+L"], S)
p.m("transport.play_in_out", ["/"], S, "Play Selection")
p.m("transport.play_around", ["Shift+/"], S, "Play Around (Shift+?)")
p.m("mark.in", ["I"], S)
p.m("mark.out", ["O"], S)
p.m("mark.clip", ["X"], S)
p.m("mark.clear_in", ["Alt+I"], S)
p.m("mark.clear_out", ["Alt+O"], S)
p.m("mark.clear_both", ["Alt+X"], S)
p.m("mark.go_to_in", ["Shift+I"], S)
p.m("mark.go_to_out", ["Shift+O"], S)
p.m("mark.add_marker", ["M"], S)
p.m("mark.modify_marker", ["Alt+M"], L, "Add Marker and Modify")
p.m("mark.delete_marker", ["Meta+M"], L, "Control+M")
p.m("transport.prev_marker", ["Meta+;"], S, "Control+;")
p.m("transport.next_marker", ["Meta+'"], S, "Control+'")
p.m("edit.insert", ["W"], S)
p.m("edit.overwrite", ["D"], S)
p.m("edit.append_at_end", ["E"], S)
p.m("edit.place_on_top", ["Q"], L, "Connect; the clip goes on the track above")
p.m("edit.replace", ["Shift+R"], S)
p.m("edit.split", ["Ctrl+B"], S, "Blade at the playhead")
p.m("edit.ripple_delete", ["Backspace", "Del"], S, "Delete in the magnetic timeline closes the gap")
p.m("edit.delete", ["Shift+Backspace"], S, "Replace with Gap")
p.m("edit.paste_attributes", ["Ctrl+Shift+V"], S)
p.m("edit.deselect_all", ["Ctrl+Shift+A"], S)
p.m("tool.select", ["A"], S)
p.m("tool.trim", ["T"], S)
p.m("tool.blade", ["B"], S)
p.m("trim.extend_edit", ["Shift+X"], S)
p.m("trim.nudge_left", [","], S)
p.m("trim.nudge_right", ["."], S)
p.m("trim.nudge_left_multi", ["Shift+,"], S)
p.m("trim.nudge_right_multi", ["Shift+."], S)
p.m("trim.trim_start", ["Alt+["], S)
p.m("trim.trim_end", ["Alt+]"], S)
p.m("timeline.snapping", ["N"], S)
p.m("timeline.audio_scrubbing", ["Shift+S"], S, "Audio Skimming")
p.m("timeline.zoom_in", ["Ctrl+="], S)
p.m("timeline.zoom_out", ["Ctrl+-"], S)
p.m("timeline.zoom_fit", ["Shift+Z"], S)
p.m("timeline.add_transition", ["Ctrl+T"], S)
p.m("timeline.new", ["Ctrl+N"], S, "New Project (a timeline in Final Cut)")
p.m("timeline.settings", ["Ctrl+J"], L, "Project Properties")
p.m("clip.toggle_enabled", ["V"], S)
p.m("clip.match_frame", ["Shift+F"], S, "Reveal in Browser")
p.m("clip.flag", ["F"], U, "Favourite in Final Cut; Kinoklip flags the clip")
p.m("clip.retime_controls", ["Ctrl+R"], S, "Retime Editor")
p.m("clip.change_duration", ["Meta+D"], S, "Control+D")
p.m("clip.freeze_frame", ["Alt+F"], S, "Add Freeze Frame")
p.m("clip.new_compound", ["Alt+G"], S)
p.m("clip.decompose", ["Ctrl+Shift+G"], S, "Break Apart Clip Items")
p.m("clip.unlink", ["Meta+Shift+S"], L, "Detach Audio (Control+Shift+S)")
p.m("clip.add_keyframe", ["Alt+K"], S)
p.m("clip.prev_keyframe", ["Alt+;"], S)
p.m("clip.next_keyframe", ["Alt+'"], S)
p.m("file.import_media", ["Ctrl+I"], S)
p.m("mediapool.new_bin", ["Ctrl+Shift+N"], L, "New Folder")
p.m("edit.preferences", ["Ctrl+,"], S)
p.m("edit.keyboard_customization", ["Ctrl+Alt+K"], S, "Command Editor")
p.m("view.page_deliver", ["Ctrl+E"], L, "Share (Export File); Kinoklip's is the Deliver page")
p.m("view.fullscreen_viewer", ["Ctrl+Shift+F"], S, "Play Full Screen")
p.m("view.fullscreen", ["Ctrl+Meta+F"], L, "Enter Full Screen (Control+Command+F)")
p.m("view.toggle_media_pool", ["Ctrl+1"], L, "Browser")
p.m("view.toggle_inspector", ["Ctrl+4"], S, "Inspector")
p.m("view.toggle_effects", ["Ctrl+5"], S, "Effects Browser")
p.m("view.video_scopes", ["Ctrl+7"], S, "Video Scopes")

# ---- Avid Media Composer (Windows) ----
p = preset("avid-media-composer", "Avid Media Composer", "Media Composer",
    ["Media Composer's default Windows keyboard: the letter keys of the left hand mark, move and",
     "edit (E / R mark In / Out, Q / W go to them, A / S go to the previous / next edit, Z lifts,",
     "X extracts, V splices in, B overwrites) and M , . / trim. I and O mark too."],
    ["Segment Mode (red and yellow arrows) and Lift / Overwrite or Extract / Splice-in segment drags: drag clips on the timeline instead.",
     "Top / Tail (trim to the playhead and ripple) are Kinoklip's Ripple Trim Start / End, without a default key here.",
     "Locators: the Add Locator key varies between Media Composer keyboards; F5 is a guess.",
     "Match Frame, Step In / Out, Mark / Clear keys of the source monitor, Quick Transition options, Ctrl+] / Ctrl+[ track sizes.",
     "Snapping has no key in Media Composer (it is Ctrl-drag); N stays."])
common(p, quit=False)
p.m("edit.redo", ["Ctrl+R"], L)
p.m("transport.shuttle_reverse", ["J"], S)
p.m("transport.shuttle_stop", ["K"], S)
p.m("transport.shuttle_forward", ["L"], S)
p.m("mark.in", ["I", "E"], S)
p.m("mark.out", ["O", "R"], S)
p.m("mark.clip", ["T"], S)
p.m("mark.clear_in", ["D"], S)
p.m("mark.clear_out", ["F"], S)
p.m("mark.clear_both", ["G"], S)
p.m("mark.go_to_in", ["Q"], S)
p.m("mark.go_to_out", ["W"], S)
p.m("transport.prev_edit", ["A", "Up"], S, "Fast Back / Go to Previous Edit; Up kept")
p.m("transport.next_edit", ["S", "Down"], S, "Fast Forward / Go to Next Edit; Down kept")
p.m("edit.delete", ["Z", "Backspace"], S, "Lift")
p.m("edit.ripple_delete", ["X", "Del"], S, "Extract")
p.m("edit.insert", ["V"], S, "Splice-in")
p.m("edit.overwrite", ["B"], S)
p.m("edit.split", ["H"], U, "Add Edit; its key varies between keyboard settings")
p.m("tool.trim", ["U"], L, "Trim Mode")
p.m("trim.nudge_left", [","], S, "Trim Left one frame")
p.m("trim.nudge_right", ["."], S, "Trim Right one frame")
p.m("trim.nudge_left_multi", ["M"], S, "Trim Left ten frames")
p.m("trim.nudge_right_multi", ["/"], S, "Trim Right ten frames")
p.m("view.toggle_source_timeline", ["Y"], L, "toggles between the source and record monitor")
p.m("timeline.zoom_in", ["Ctrl+]"], S)
p.m("timeline.zoom_out", ["Ctrl+["], S)
p.m("timeline.add_transition", ["\\"], L, "Quick Transition")
p.m("mark.add_marker", ["F5"], U, "Add Locator; varies between keyboards")
p.m("mediapool.new_bin", ["Ctrl+N"], S)
p.m("timeline.new", ["Ctrl+Shift+N"], L, "New Sequence")

# ---- Kdenlive ----
p = preset("kdenlive", "Kdenlive", "Kdenlive",
    ["Kdenlive's default keys (it uses the KDE standard keys for the file, edit and settings",
     "commands). Shift+R cuts the clip at the playhead; S and X pick the Selection and Razor",
     "tools; V and B insert and overwrite the clip zone."],
    ["Spacer (M), Slip, Ripple and Multicam tools; track selection with Up / Down (here they go to the previous / next edit, as in Resolve).",
     "Insert Space, Remove Space, Extract / Lift Zone keys, Switch Monitor (T), Add Clip Marker with a comment.",
     "Kdenlive's Ripple Delete has no default key; Shift+Backspace stays.",
     "Previous / next marker and trim to the playhead: no default keys known."])
common(p)
p.m("edit.redo", ["Ctrl+Shift+Z"], S)
p.m("transport.shuttle_reverse", ["J"], S)
p.m("transport.shuttle_stop", ["K"], S)
p.m("transport.shuttle_forward", ["L"], S)
p.m("transport.back_1s", ["Shift+Left"], L, "one second")
p.m("transport.forward_1s", ["Shift+Right"], L)
p.m("transport.prev_edit", ["Alt+Left", "Up"], L, "previous snap point; Up kept")
p.m("transport.next_edit", ["Alt+Right", "Down"], L, "next snap point; Down kept")
p.m("transport.play_in_out", ["Ctrl+Space"], L, "Play Zone")
p.m("mark.in", ["I"], S)
p.m("mark.out", ["O"], S)
p.m("edit.split", ["Shift+R"], S, "Cut Clip at the playhead")
p.m("tool.select", ["S"], S)
p.m("tool.blade", ["X"], S, "Razor tool")
p.m("edit.insert", ["V"], L, "Insert Clip Zone")
p.m("edit.overwrite", ["B"], L, "Overwrite Clip Zone")
p.m("edit.delete", ["Del"], S, "Delete Selected Item")
p.m("timeline.zoom_in", ["Ctrl+="], S)
p.m("timeline.zoom_out", ["Ctrl+-"], S)
p.m("timeline.zoom_fit", ["Ctrl+0", "Shift+Z"], U, "Fit Zoom to Project; Shift+Z kept")
p.m("clip.link", ["Ctrl+G"], S, "Group Clips")
p.m("clip.unlink", ["Ctrl+Shift+G"], S, "Ungroup Clips")
p.m("file.new", ["Ctrl+N"], S)
p.m("file.save_as", ["Ctrl+Shift+S"], S)
p.m("view.page_deliver", ["Ctrl+Return"], L, "Render")
p.m("view.fullscreen", ["Ctrl+Shift+F"], S, "KDE's Full Screen Mode")
p.m("edit.preferences", ["Ctrl+Shift+,"], S, "KDE's Configure")
p.m("edit.keyboard_customization", ["Ctrl+Alt+,"], L, "KDE's Configure Keyboard Shortcuts")

# ---- Shotcut ----
p = preset("shotcut", "Shotcut", "Shotcut",
    ["Shotcut's default keys: S splits at the playhead, X ripple deletes and Z lifts, A appends,",
     "V inserts and B overwrites the source clip. Shotcut has no tools and no pages, so those",
     "keep Kinoklip's keys or are left free where Shotcut uses the letter."],
    ["Ripple mode, Ripple All Tracks and Scrub While Dragging toggles; Shotcut's track keys (Ctrl+Up / Down, track height).",
     "Copy Clip (C) into the source player, Nudge with Alt+, / Alt+. in recent versions (not sure), Paste Filters.",
     "Previous / next marker keys, trim to the playhead keys: not known well enough to map."])
common(p)
p.m("edit.redo", ["Ctrl+Shift+Z", "Ctrl+Y"], S)
p.m("transport.shuttle_reverse", ["J"], S)
p.m("transport.shuttle_stop", ["K"], S)
p.m("transport.shuttle_forward", ["L"], S)
p.m("mark.in", ["I"], S)
p.m("mark.out", ["O"], S)
p.m("edit.split", ["S"], S, "Split at Playhead")
p.m("edit.ripple_delete", ["X", "Del"], L, "Ripple Delete; Delete kept")
p.m("edit.delete", ["Z", "Backspace"], L, "Lift; Backspace kept")
p.m("edit.append_at_end", ["A"], S)
p.m("edit.insert", ["V"], L)
p.m("edit.overwrite", ["B"], L)
p.m("mark.add_marker", ["M"], L)
p.m("timeline.zoom_in", ["=", "Ctrl+="], L)
p.m("timeline.zoom_out", ["-", "Ctrl+-"], L)
p.m("timeline.zoom_fit", ["0"], L, "Zoom Timeline to Fit")
p.m("file.new", ["Ctrl+N"], S)
p.m("file.save_as", ["Ctrl+Shift+S"], S)
p.m("view.fullscreen", ["F11"], L)

# ---- Build each preset ----

def build(p):
    """Returns (shortcuts, report) for preset p."""
    used = []  # (key, scope, id) of the mapped rows
    for aid, (keys, conf, note) in p.rows.items():
        for k in keys:
            for k2, sc2, aid2 in used:
                if k == k2 and scopes_conflict(scope(aid), sc2):
                    sys.exit(f"{p.id}: {k} mapped to both {aid2} and {aid}")
            used.append((k, scope(aid), aid))
    shortcuts = collections.OrderedDict()
    report = collections.OrderedDict()  # id -> (kind, keys, conf, note); kind mapped/kept/freed/none
    for aid, row in ACTIONS.items():
        rkeys = row[2]
        if aid in p.rows:
            keys, conf, note = p.rows[aid]
            shortcuts[aid] = keys
            report[aid] = ("mapped", keys, conf, note)
            continue
        kept, lost = [], []
        for k in rkeys:
            owner = next((a2 for k2, sc2, a2 in used if k2 == k and scopes_conflict(scope(aid), sc2)), None)
            (lost if owner else kept).append((k, owner))
        keys = [k for k, _ in kept]
        shortcuts[aid] = keys
        if lost:
            what = "; ".join(f"{k} is {ACTIONS[o][1].split(' (')[0]} here" for k, o in lost)
            report[aid] = ("kept" if keys else "freed", keys, "", what)
        elif keys:
            report[aid] = ("kept", keys, "", "")
        else:
            report[aid] = ("none", [], "", "")
    # Final check over every key, like test_keypresets.
    seen = []
    for aid, keys in shortcuts.items():
        for k in keys:
            for k2, sc2, a2 in seen:
                if k == k2 and scopes_conflict(scope(aid), sc2):
                    sys.exit(f"{p.id}: {k} on both {a2} and {aid}")
            seen.append((k, scope(aid), aid))
    return shortcuts, report

def md_keys(k):
    return " / ".join("`" + x.replace("|", "\\|") + "`" for x in k) if k else "(none)"

def write_json(p, shortcuts):
    comment = (f"Built-in preset following {p.editor}'s default shortcuts, documented with a confidence "
        f"per key in docs/keymaps/{p.id}.md; generated by scripts/keymap-presets.py. Commands without a "
        f"counterpart keep the default profile's key, or none where {p.editor} uses that key. Read-only: "
        "users rebind in Keyboard Customization.")
    lines = ["{", '    "formatVersion": 1,', '    "name": ' + json.dumps(p.name) + ",",
        '    "comment": ' + json.dumps(comment) + ",", '    "shortcuts": {']
    items = list(shortcuts.items())
    for i, (k, v) in enumerate(items):
        lines.append("        " + json.dumps(k) + ": " + json.dumps(v, ensure_ascii=False) + ("," if i + 1 < len(items) else ""))
    lines += ["    }", "}"]
    with open(os.path.join(root, "resources", "keymaps", p.id + ".json"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

def counts(report):
    c = collections.Counter()
    for kind, keys, conf, note in report.values():
        c[kind] += 1
        if kind == "mapped":
            c[conf] += 1
    return c

def write_doc(p, report):
    c = counts(report)
    out = [f"# Keymap: {p.name}", "",
        f"A built-in keyboard preset (`resources/keymaps/{p.id}.json`, profile id `{p.id}`) for people",
        f"coming from {p.editor}. Choose it in Kinoklip menu > Keyboard Customization > Keyboard",
        "profile (or in the Project Manager the first time Kinoklip starts). Written from general",
        f"knowledge of how {p.editor} is used; nothing is copied from its documentation.", ""]
    out += p.intro + [""]
    out += ["**Confidence** says how sure we are that the key is that editor's default:",
        "- **sure**: well-known default, used daily;",
        "- **likely**: believed correct, not checked against the program;",
        "- **unsure**: best guess; please correct it in `scripts/keymap-presets.py` if it differs.", "",
        f"Counts: {len(report)} commands; {c['mapped']} mapped to {p.editor}'s key (sure {c[S]}, likely {c[L]},",
        f"unsure {c[U]}); {c['kept']} keep the default (Resolve) key; {c['freed']} have no key because",
        f"{p.editor} uses it for something else; {c['none']} have no key in either.", "",
        "To change a key: edit the preset's section in `scripts/keymap-presets.py` and run it (it writes",
        "the JSON file and this page and stops when a key is used twice in one scope);",
        "`test_keypresets` checks every preset the same way.", "",
        f"## Mapped to {p.editor}", "",
        "| Command | Action id | Key | Confidence | Note |", "|---|---|---|---|---|"]
    for aid, (kind, keys, conf, note) in report.items():
        if kind == "mapped":
            out.append(f"| {ACTIONS[aid][1]} | `{aid}` | {md_keys(keys)} | {conf} | {note} |")
    out += ["", f"## Unbound: the key does something else in {p.editor}", "",
        "Rebind these in Keyboard Customization if you use them.", "",
        "| Command | Action id | Default key | Why |", "|---|---|---|---|"]
    for aid, (kind, keys, conf, note) in report.items():
        if kind == "freed":
            out.append(f"| {ACTIONS[aid][1]} | `{aid}` | {md_keys(ACTIONS[aid][2])} | {note} |")
    out += ["", "## Keeping the default (Resolve) key", "",
        f"No {p.editor} counterpart is known, so these keep Kinoklip's default key (a key it",
        f"shares with a {p.editor} command was removed, see the note).", "",
        "| Command | Action id | Key | Note |", "|---|---|---|---|"]
    for aid, (kind, keys, conf, note) in report.items():
        if kind == "kept":
            out.append(f"| {ACTIONS[aid][1]} | `{aid}` | {md_keys(keys)} | {note} |")
    out += ["", f"## {p.editor} commands Kinoklip does not have (gaps)", ""]
    out += [f"- {g}" for g in p.gaps]
    with open(os.path.join(root, "docs", "keymaps", p.id + ".md"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

def write_index(summaries):
    out = ["# Keyboard presets", "",
        "Kinoklip's default keys follow [DaVinci Resolve](https://www.blackmagicdesign.com/products/davinciresolve) on Windows ([its table](../keymap-resolve-windows.md)).",
        "These built-in presets follow other editors instead; each page lists every key with a",
        "confidence and the gaps. They are generated by `scripts/keymap-presets.py`.", "",
        "| Preset | Profile id | Mapped | of which unsure | Default key kept | Unbound |", "|---|---|---|---|---|---|"]
    for p, c in summaries:
        out.append(f"| [{p.name}]({p.id}.md) | `{p.id}` | {c['mapped']} | {c[U]} | {c['kept']} | {c['freed']} |")
    out += ["", "Import of your own files (Keyboard Customization > Import...): Premiere Pro `.kys` and Final",
        "Cut Pro `.commandset` (XML plist) files are read too. The commands Kinoklip recognises get",
        "their keys on top of the matching preset; unknown entries are skipped and counted.",
        "See `src/ui/common/ForeignKeymaps.cpp`."]
    with open(os.path.join(root, "docs", "keymaps", "readme.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

def main():
    os.makedirs(os.path.join(root, "docs", "keymaps"), exist_ok=True)
    summaries = []
    for p in PRESETS:
        shortcuts, report = build(p)
        write_json(p, shortcuts)
        write_doc(p, report)
        c = counts(report)
        summaries.append((p, c))
        print(f"{p.id}: mapped {c['mapped']} (sure {c[S]}, likely {c[L]}, unsure {c[U]}), "
              f"kept {c['kept']}, unbound {c['freed']}, none {c['none']}, gaps listed {len(p.gaps)}")
    write_index(summaries)

if __name__ == "__main__":
    main()
