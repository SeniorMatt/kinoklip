# Keymap: Vim

A built-in keyboard preset (`resources/keymaps/vim.json`, profile id `vim`) for people
coming from Vim and Neovim. Choose it in Kinoklip menu > Keyboard Customization > Keyboard
profile (or in the Project Manager the first time Kinoklip starts). Written from general
knowledge of how Vim and Neovim is used; nothing is copied from its documentation.

A keyboard preset for people who edit text with Vim or
Neovim. The keys are not shortcuts: while the **timeline or a viewer has
the focus** a small input layer reads them as vim does, with counts (`5l`), two-key
sequences (`gg`, `dd`, `yy`) and modes (NORMAL, VISUAL, shown in the timeline's tool bar).
It is off in every text field, the Inspector, dialogs and the title editor, and it exists
only while this preset is chosen; every other preset behaves exactly as before. Every
key drives an existing action (a Ripple Delete is the ordinary Ripple Delete, one undo step).

All other commands keep their Resolve keys. Keys the layer takes (every key typed without
Ctrl, Alt or Meta) move to **Alt + the same key** where that is free, so nothing is lost;
the rest are listed below as unbound.

**Confidence** says how sure we are that the key is that editor's default:
- **sure**: well-known default, used daily;
- **likely**: believed correct, not checked against the program;
- **unsure**: best guess; please correct it in `scripts/keymap-presets.py` if it differs.

Counts: 344 commands; 4 mapped to Vim and Neovim's key (sure 1, likely 3,
unsure 0); 130 keep the default (Resolve) key; 17 have no key because
Vim and Neovim uses it for something else; 157 have no key in either.

To change a key: edit the preset's section in `scripts/keymap-presets.py` and run it (it writes
the JSON file and this page and stops when a key is used twice in one scope);
`test_keypresets` checks every preset the same way.

## Mapped to Vim and Neovim

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Redo | `edit.redo` | `Ctrl+R` / `Ctrl+Shift+Z` | sure | vim's redo; Ctrl+Shift+Z kept |
| Shuttle Reverse (J, again = faster) | `transport.shuttle_reverse` | `Alt+J` | likely | J is typed to the layer |
| Stop (K; K+J / K+L step and slow play) | `transport.shuttle_stop` | `Alt+Shift+K` | likely | Alt+K is Add Keyframe |
| Shuttle Forward (L, again = faster) | `transport.shuttle_forward` | `Alt+L` | likely | L is typed to the layer |

## Unbound: the key does something else in Vim and Neovim

Rebind these in Keyboard Customization if you use them.

| Command | Action id | Default key | Why |
|---|---|---|---|
| Select Clips Forward (auto-select tracks) | `edit.select_forward` | `Y` | Y is typed to the vim layer here |
| Select Nearest Edit Point | `trim.select_nearest_edit` | `V` | V is typed to the vim layer here |
| Match Frame (load the source at this frame) | `clip.match_frame` | `F` | F is typed to the vim layer here |
| Retime Controls (speed bar on the selected clips) | `clip.retime_controls` | `Ctrl+R` | Ctrl+R is Redo here |
| Mark In | `mark.in` | `I` | I is typed to the vim layer here |
| Mark Out | `mark.out` | `O` | O is typed to the vim layer here |
| Mark Clip | `mark.clip` | `X` | X is typed to the vim layer here |
| Add Marker (on the selected clip: clip marker) | `mark.add_marker` | `M` | M is typed to the vim layer here |
| Play Around Current Frame | `transport.play_around` | `/` | / is typed to the vim layer here |
| Cut / Switch to Multicam Angle 1 | `multicam.angle_1` | `1` | 1 is typed to the vim layer here |
| Cut / Switch to Multicam Angle 2 | `multicam.angle_2` | `2` | 2 is typed to the vim layer here |
| Cut / Switch to Multicam Angle 3 | `multicam.angle_3` | `3` | 3 is typed to the vim layer here |
| Cut / Switch to Multicam Angle 4 | `multicam.angle_4` | `4` | 4 is typed to the vim layer here |
| Cut / Switch to Multicam Angle 5 | `multicam.angle_5` | `5` | 5 is typed to the vim layer here |
| Cut / Switch to Multicam Angle 6 | `multicam.angle_6` | `6` | 6 is typed to the vim layer here |
| Cut / Switch to Multicam Angle 7 | `multicam.angle_7` | `7` | 7 is typed to the vim layer here |
| Cut / Switch to Multicam Angle 8 | `multicam.angle_8` | `8` | 8 is typed to the vim layer here |

## Keeping the default (Resolve) key

No Vim and Neovim counterpart is known, so these keep Kinoklip's default key (a key it
shares with a Vim and Neovim command was removed, see the note).

| Command | Action id | Key | Note |
|---|---|---|---|
| New Timeline | `timeline.new` | `Ctrl+N` |  |
| Open Project (file dialog) | `file.open` | `Ctrl+O` |  |
| Save Project | `file.save` | `Ctrl+S` |  |
| Save Project As | `file.save_as` | `Ctrl+Shift+S` |  |
| Import Media | `file.import_media` | `Ctrl+I` |  |
| Quit | `file.quit` | `Ctrl+Q` |  |
| Undo | `edit.undo` | `Ctrl+Z` |  |
| Cut | `edit.cut` | `Ctrl+X` |  |
| Copy | `edit.copy` | `Ctrl+C` |  |
| Paste | `edit.paste` | `Ctrl+V` |  |
| Paste Insert | `edit.paste_insert` | `Ctrl+Shift+V` |  |
| Paste Attributes | `edit.paste_attributes` | `Alt+V` |  |
| Ripple Cut | `edit.ripple_cut` | `Ctrl+Shift+X` |  |
| Delete Selected (lift, leaves a gap) | `edit.delete` | `Backspace` |  |
| Ripple Delete | `edit.ripple_delete` | `Del` / `Shift+Backspace` |  |
| Select All | `edit.select_all` | `Ctrl+A` |  |
| Deselect All | `edit.deselect_all` | `Ctrl+Shift+A` |  |
| Select Clips Backward (auto-select tracks) | `edit.select_backward` | `Ctrl+Y` |  |
| Select Clips Forward on All Tracks | `edit.select_forward_all` | `Alt+Y` |  |
| Select Clips Backward on All Tracks | `edit.select_backward_all` | `Ctrl+Alt+Y` |  |
| Insert Clip | `edit.insert` | `F9` |  |
| Overwrite Clip | `edit.overwrite` | `F10` |  |
| Ripple Overwrite | `edit.ripple_overwrite` | `Shift+F10` |  |
| Replace Clip | `edit.replace` | `F11` |  |
| Fit to Fill | `edit.fit_to_fill` | `Shift+F11` |  |
| Place on Top | `edit.place_on_top` | `F12` |  |
| Append at End | `edit.append_at_end` | `Shift+F12` |  |
| Keyboard Customization | `edit.keyboard_customization` | `Ctrl+Alt+K` |  |
| Preferences | `edit.preferences` | `Ctrl+,` |  |
| Ripple Trim Start to Playhead (closes the gap) | `trim.ripple_trim_start` | `Ctrl+Shift+[` |  |
| Ripple Trim End to Playhead (closes the gap) | `trim.ripple_trim_end` | `Ctrl+Shift+]` |  |
| Swap Clip with Previous | `trim.swap_left` | `Ctrl+Shift+,` |  |
| Swap Clip with Next | `trim.swap_right` | `Ctrl+Shift+.` |  |
| Move Clips Up a Track (video clips of the selection, else its audio) | `trim.move_up` | `Alt+Up` |  |
| Move Clips Down a Track | `trim.move_down` | `Alt+Down` |  |
| Split Clip (Razor) at Playhead | `edit.split` | `Ctrl+\` |  |
| Join Clips (remove a through edit) | `edit.join` | `Alt+\` |  |
| Add Default Transition | `timeline.add_transition` | `Ctrl+T` |  |
| Linked Selection | `timeline.linked_selection` | `Ctrl+Shift+L` |  |
| Zoom In | `timeline.zoom_in` | `Ctrl+=` |  |
| Zoom Out | `timeline.zoom_out` | `Ctrl+-` |  |
| Video Destination V1 | `timeline.target_video_1` | `Alt+1` |  |
| Video Destination V2 | `timeline.target_video_2` | `Alt+2` |  |
| Video Destination V3 | `timeline.target_video_3` | `Alt+3` |  |
| Video Destination V4 | `timeline.target_video_4` | `Alt+4` |  |
| Video Destination V5 | `timeline.target_video_5` | `Alt+5` |  |
| Video Destination V6 | `timeline.target_video_6` | `Alt+6` |  |
| Video Destination V7 | `timeline.target_video_7` | `Alt+7` |  |
| Video Destination V8 | `timeline.target_video_8` | `Alt+8` |  |
| Audio Destination A1 | `timeline.target_audio_1` | `Ctrl+Alt+1` |  |
| Audio Destination A2 | `timeline.target_audio_2` | `Ctrl+Alt+2` |  |
| Audio Destination A3 | `timeline.target_audio_3` | `Ctrl+Alt+3` |  |
| Audio Destination A4 | `timeline.target_audio_4` | `Ctrl+Alt+4` |  |
| Audio Destination A5 | `timeline.target_audio_5` | `Ctrl+Alt+5` |  |
| Audio Destination A6 | `timeline.target_audio_6` | `Ctrl+Alt+6` |  |
| Audio Destination A7 | `timeline.target_audio_7` | `Ctrl+Alt+7` |  |
| Audio Destination A8 | `timeline.target_audio_8` | `Ctrl+Alt+8` |  |
| Toggle Auto Select V1 | `timeline.auto_select_video_1` | `Ctrl+1` |  |
| Toggle Auto Select V2 | `timeline.auto_select_video_2` | `Ctrl+2` |  |
| Toggle Auto Select V3 | `timeline.auto_select_video_3` | `Ctrl+3` |  |
| Toggle Auto Select V4 | `timeline.auto_select_video_4` | `Ctrl+4` |  |
| Toggle Auto Select V5 | `timeline.auto_select_video_5` | `Ctrl+5` |  |
| Toggle Auto Select V6 | `timeline.auto_select_video_6` | `Ctrl+6` |  |
| Toggle Auto Select V7 | `timeline.auto_select_video_7` | `Ctrl+7` |  |
| Toggle Auto Select V8 | `timeline.auto_select_video_8` | `Ctrl+8` |  |
| Toggle Auto Select A1 | `timeline.auto_select_audio_1` | `Ctrl+Shift+1` |  |
| Toggle Auto Select A2 | `timeline.auto_select_audio_2` | `Ctrl+Shift+2` |  |
| Toggle Auto Select A3 | `timeline.auto_select_audio_3` | `Ctrl+Shift+3` |  |
| Toggle Auto Select A4 | `timeline.auto_select_audio_4` | `Ctrl+Shift+4` |  |
| Toggle Auto Select A5 | `timeline.auto_select_audio_5` | `Ctrl+Shift+5` |  |
| Toggle Auto Select A6 | `timeline.auto_select_audio_6` | `Ctrl+Shift+6` |  |
| Toggle Auto Select A7 | `timeline.auto_select_audio_7` | `Ctrl+Shift+7` |  |
| Toggle Auto Select A8 | `timeline.auto_select_audio_8` | `Ctrl+Shift+8` |  |
| Change Clip Duration | `clip.change_duration` | `Ctrl+D` |  |
| Retime Curve (opens the clip's retime curve in the curve editor) | `clip.retime_curve` | `Ctrl+Shift+R` |  |
| Link / Unlink Clips | `clip.link` | `Ctrl+Alt+L` |  |
| Add Keyframe (inspector) | `clip.add_keyframe` | `Alt+K` |  |
| Clear In | `mark.clear_in` | `Alt+I` |  |
| Clear Out | `mark.clear_out` | `Alt+O` |  |
| Clear In and Out | `mark.clear_both` | `Alt+X` |  |
| Add and Modify Marker | `mark.modify_marker` | `Ctrl+M` |  |
| Delete Marker | `mark.delete_marker` | `Alt+M` |  |
| Viewer Full Screen | `view.fullscreen_viewer` | `Ctrl+F` |  |
| Full Screen Window | `view.fullscreen` | `Ctrl+Shift+F` |  |
| Play / Stop | `transport.play_pause` | `Space` |  |
| Play Reverse | `transport.play_reverse` | `Shift+Space` |  |
| Record Voice-Over (count-in, then record the armed audio track while the timeline plays) | `transport.record` | `Ctrl+Alt+R` |  |
| Previous Frame | `transport.prev_frame` | `Left` |  |
| Next Frame | `transport.next_frame` | `Right` |  |
| Back One Second | `transport.back_1s` | `Shift+Left` |  |
| Forward One Second | `transport.forward_1s` | `Shift+Right` |  |
| Previous Edit | `transport.prev_edit` | `Up` |  |
| Next Edit | `transport.next_edit` | `Down` |  |
| Go to Timeline Start | `transport.go_to_start` | `Home` |  |
| Go to Timeline End | `transport.go_to_end` | `End` |  |
| Previous Marker | `transport.prev_marker` | `Shift+Up` |  |
| Next Marker | `transport.next_marker` | `Shift+Down` |  |
| Loop | `transport.loop` | `Ctrl+/` |  |
| Play In to Out | `transport.play_in_out` | `Alt+/` |  |
| New Bin | `mediapool.new_bin` | `Ctrl+Shift+N` |  |
| Video Scopes (floating window: waveform, parade, vectorscope, histogram) | `view.video_scopes` | `Ctrl+Shift+W` |  |
| Find in Media Pool (timeline clip) | `timeline.find_in_media_pool` | `Alt+F` |  |
| Add Node (searchable Select Node list; adds after the selected node) | `comp.select_node` | `Shift+Space` |  |
| Cut Nodes | `comp.cut` | `Ctrl+X` |  |
| Copy Nodes | `comp.copy` | `Ctrl+C` |  |
| Paste Nodes | `comp.paste` | `Ctrl+V` |  |
| Duplicate Nodes | `comp.duplicate` | `Ctrl+D` |  |
| Delete Nodes | `comp.delete` | `Del` / `Backspace` |  |
| Select All Nodes | `comp.select_all` | `Ctrl+A` |  |
| Rename Node | `comp.rename` | `F2` |  |
| Pass Through (disable the node: its main input goes straight through) | `comp.pass_through` | `Ctrl+P` |  |
| Swap Inputs (background and foreground of Merge / Dissolve) | `comp.swap_inputs` | `Ctrl+T` |  |
| View Node in Left Viewer | `comp.view_left` | `1` |  |
| View Node in Right Viewer | `comp.view_right` | `2` |  |
| Fit Nodes to View | `comp.fit` | `Ctrl+F` |  |
| Add Serial Node (after the current node) | `color.add_serial` | `Alt+S` |  |
| Add Parallel Node (with a Parallel mixer) | `color.add_parallel` | `Alt+P` |  |
| Add Layer Node (with a Layer mixer) | `color.add_layer` | `Alt+L` |  |
| Add Serial Node Before (the current node) | `color.add_serial_before` | `Shift+S` |  |
| Delete Node (the current node) | `color.delete_node` | `Del` / `Backspace` |  |
| Enable / Disable Current Node | `color.bypass_node` | `Ctrl+D` |  |
| Bypass All Grades (viewer only; the render keeps them) | `color.bypass_all` | `Shift+D` |  |
| Reset Node Grade (the current node's corrections) | `color.reset_node` | `Shift+Home` |  |
| Reset Clip Grade (back to one empty node 01) | `color.reset_grade` | `Ctrl+Home` |  |
| Grab Still (the current frame and grade into the gallery) | `color.grab_still` | `Ctrl+Alt+G` |  |
| Copy Grade (of the current clip) | `color.copy_grade` | `Ctrl+C` |  |
| Paste Grade (to the selected clips of the clips strip) | `color.paste_grade` | `Ctrl+V` |  |
| Highlight (show the current node's key in the viewer) | `color.highlight` | `Shift+H` |  |
| Wipe with Still (split the viewer with the selected gallery still) | `color.wipe` | `Ctrl+W` |  |
| Help for This (opens the user guide's article for the effect, panel or page you are on) | `help.guide_context` | `F1` |  |

## Keys moved to Alt

The layer takes every key typed without Ctrl, Alt or Meta while a timeline or viewer has the
focus, so these commands moved to Alt + their key (or kept no key where that is taken).

| Command | Action id | Keys now | What happened |
|---|---|---|---|
| Project Manager | `file.project_manager` | `Alt+Shift+1` | Shift+1 moved to Alt+Shift+1 |
| Project Settings | `file.project_settings` | `Alt+Shift+9` | Shift+9 moved to Alt+Shift+9 |
| Selection Mode | `tool.select` | `Alt+A` | A moved to Alt+A |
| Trim Edit Mode | `tool.trim` | `Alt+T` | T moved to Alt+T |
| Blade Edit Mode | `tool.blade` | `Alt+B` | B moved to Alt+B |
| Dynamic Trim Mode (J/K/L trim the selected edit) | `trim.dynamic` | `Alt+W` | W moved to Alt+W |
| Nudge / Trim One Frame Left | `trim.nudge_left` | `Alt+,` | , moved to Alt+, |
| Nudge / Trim One Frame Right | `trim.nudge_right` | `Alt+.` | . moved to Alt+. |
| Nudge / Trim Several Frames Left | `trim.nudge_left_multi` | `Alt+Shift+,` | Shift+, moved to Alt+Shift+, |
| Nudge / Trim Several Frames Right | `trim.nudge_right_multi` | `Alt+Shift+.` | Shift+. moved to Alt+Shift+. |
| Trim Start to Playhead (leaves a gap) | `trim.trim_start` | `Alt+Shift+[` | Shift+[ moved to Alt+Shift+[ |
| Trim End to Playhead (leaves a gap) | `trim.trim_end` | `Alt+Shift+]` | Shift+] moved to Alt+Shift+] |
| Extend Edit to Playhead | `trim.extend_edit` | `Alt+E` | E moved to Alt+E |
| Toggle Edit Point Side (out / roll / in) | `trim.cycle_edit` | `Alt+U` | U moved to Alt+U |
| Select Nearest Clip or Gap | `trim.select_nearest_clip` | `Alt+Shift+V` | Shift+V moved to Alt+Shift+V |
| Snapping | `timeline.snapping` | `Alt+N` | N moved to Alt+N |
| Audio Scrubbing (a short sound while the playhead is dragged or stepped) | `timeline.audio_scrubbing` | `Alt+Shift+S` | Shift+S moved to Alt+Shift+S |
| Zoom to Fit | `timeline.zoom_fit` | `Alt+Shift+Z` | Shift+Z moved to Alt+Shift+Z |
| Enable / Disable Clip | `clip.toggle_enabled` | `Alt+D` | D moved to Alt+D |
| Change Clip Speed | `clip.change_speed` | `Alt+R` | R moved to Alt+R |
| Freeze Frame (the clip shows the frame at the playhead) | `clip.freeze_frame` | `Alt+Shift+R` | Shift+R moved to Alt+Shift+R |
| Previous Keyframe | `clip.prev_keyframe` | `Alt+[` | [ moved to Alt+[ |
| Next Keyframe | `clip.next_keyframe` | `Alt+]` | ] moved to Alt+] |
| Mark Selection | `mark.selection` | `Alt+Shift+A` | Shift+A moved to Alt+Shift+A |
| Go to In | `mark.go_to_in` | `Alt+Shift+I` | Shift+I moved to Alt+Shift+I |
| Go to Out | `mark.go_to_out` | `Alt+Shift+O` | Shift+O moved to Alt+Shift+O |
| Media Page | `view.page_media` | `Alt+Shift+2` | Shift+2 moved to Alt+Shift+2 |
| Cut Page | `view.page_cut` | `Alt+Shift+3` | Shift+3 moved to Alt+Shift+3 |
| Edit Page | `view.page_edit` | `Alt+Shift+4` | Shift+4 moved to Alt+Shift+4 |
| Compositing Page (node compositing of the selected clip) | `view.page_compositing` | `Alt+Shift+5` | Shift+5 moved to Alt+Shift+5 |
| Color Page (grade the clip under the playhead) | `view.page_color` | `Alt+Shift+6` | Shift+6 moved to Alt+Shift+6 |
| Audio Page | `view.page_audio` | `Alt+Shift+7` | Shift+7 moved to Alt+Shift+7 |
| Deliver Page | `view.page_deliver` | `Alt+Shift+8` | Shift+8 moved to Alt+Shift+8 |
| Toggle Source / Timeline Viewer | `view.toggle_source_timeline` | `Alt+Q` | Q moved to Alt+Q |
| Viewer Zoom to Fit (the viewer under the pointer, else the one with focus, else all) | `view.viewer_zoom_fit` | `Alt+Z` | Z moved to Alt+Z |
| Cut / Switch to Multicam Angle 9 | `multicam.angle_9` | `Alt+9` | 9 moved to Alt+9 |

## The vim keys (timeline and viewer focus only)

Typed as in vim: an optional count, then the keys. Esc drops what was typed. The mode and the
keys typed so far show in the timeline's tool bar (`NORMAL`, `VISUAL`, `NORMAL  3d`).

### Motions (NORMAL and VISUAL; a count repeats the motion: `5l`)

| Keys | What it does | Action |
|---|---|---|
| `h` / `l` | previous / next frame | `transport.prev_frame` / `transport.next_frame` |
| `H` / `L` | one second back / forward | `transport.back_1s` / `transport.forward_1s` |
| `w` / `b` | next / previous edit point | `transport.next_edit` / `transport.prev_edit` |
| `0` / `$` | start / end of the timeline (the same as `gg` / `G`: there is no action for the edges of the clip under the playhead) | `transport.go_to_start` / `transport.go_to_end` |
| `gg` / `G` | start / end of the timeline | `transport.go_to_start` / `transport.go_to_end` |
| `n` / `N` | next / previous marker | `transport.next_marker` / `transport.prev_marker` |
| `m{a-z}` | remember the playhead position (kept until the window closes) |  |
| `'{a-z}` / `` `{a-z} `` | jump to the remembered position |  |
| `zi` / `zo` / `zf` | zoom the timeline in / out / to fit | `timeline.zoom_in` / `timeline.zoom_out` / `timeline.zoom_fit` |
| `j` / `k` | target video track one down / up (the track source edits go to; no track exists below V1) |  |

### Edits (NORMAL)

| Keys | What it does | Action |
|---|---|---|
| `x` | Ripple Delete the selection (nothing happens without a selection) | `edit.ripple_delete` |
| `X` | Delete the selection and leave a gap | `edit.delete` |
| `dd` | Ripple Delete the clip under the playhead (`3dd`: that clip and the next two on its track); the target video track is tried first, then the top-most track with a clip there | `edit.ripple_delete` |
| `yy` | copy the clip under the playhead (with a count, as `dd`) | `edit.copy` |
| `p` | paste at the playhead, inserting (rippling) | `edit.paste_insert` |
| `P` | paste at the playhead, overwriting | `edit.paste` |
| `u`, `Ctrl+R` | undo (`3u`: three steps) / redo | `edit.undo` / `edit.redo` |
| `i` | Insert edit of the source clip | `edit.insert` |
| `o` | Overwrite edit of the source clip | `edit.overwrite` |
| `a` | Append the source clip at the end | `edit.append_at_end` |
| `s` | split at the playhead | `edit.split` |
| `>` / `<` | nudge the selection one frame right / left (a count repeats) | `trim.nudge_right` / `trim.nudge_left` |
| `M` | add a marker at the playhead | `mark.add_marker` |

### Modes and the command line

| Keys | What it does | Action |
|---|---|---|
| `v` | VISUAL: selects the clips from where you were to the playhead, on every track; the motions above extend it |  |
| `d` / `x` (VISUAL) | Ripple Delete the selected clips and go back to NORMAL | `edit.ripple_delete` |
| `X` (VISUAL) | Delete them and leave a gap | `edit.delete` |
| `y` (VISUAL) | copy them and go back to NORMAL | `edit.copy` |
| `Esc`, `v` (VISUAL) | back to NORMAL; drops half-typed keys and a count; clears the VISUAL selection |  |
| `:` | the command line at the bottom of the window |  |

### The `:` command line

`:` opens a line at the bottom of the window. Return runs it, Esc closes it, Up / Down walk
through what you typed before in this session. There is no Tab completion.

| Command | What it does |
|---|---|
| `:w` | save the project (File > Save) |
| `:q` | close the window; asks about unsaved changes as usual |
| `:wq`, `:x` | save, then close |
| `:marker add --frame 120` | any command of `kinoklip commands`, written as on the command line (`marker add`, `clip split`, ...), on the open project; one undo step |

### Left out

| Vim | Why |
|---|---|
| `.` (repeat the last edit) | not implemented: the editor has no record of the last edit as a command |
| `/` (search) | the timeline has no search; Edit Index has its own search field (open it with the Edit Index panel key) |
| `d{motion}`, `y{motion}`, `c`, `r`, `~`, `e`, `f`, `t`, `%`, `*`, `q`, `@`, `J`, `K`, `O`, `A`, `I`, `S`, `C`, `R`, `Y`, `Z`, and the rest of the letters | no sensible timeline meaning: the key is taken by the layer and does nothing (as an unmapped key does in vim) |
| Insert mode (`i` starts an Insert edit, not a text mode) | there is no text to type into on the timeline |

### When the keys are not the layer's

Nothing changes in text fields (name, timecode, search, Inspector fields), dialogs, menus, the
title editor, or any other panel. Typing `dd` in a text field types two letters. Ctrl, Alt and
Meta combinations, Space, Return and the cursor keys are always ordinary shortcuts.

## Vim and Neovim commands Kinoklip does not have (gaps)

- Vim's text objects, registers, macros (`q`), the jump list, folds, windows and tabs, and Ex ranges.
- Insert mode: the timeline has no text; `i` and `a` start Insert and Append edits instead.
- `.` repeats the last change in vim; Kinoklip does not record edits as repeatable commands.
- `/` search: the timeline has none.
