# Keymap: Kdenlive

A built-in keyboard preset (`resources/keymaps/kdenlive.json`, profile id `kdenlive`) for people
coming from Kdenlive. Choose it in Kinoklip menu > Keyboard Customization > Keyboard
profile (or in the Project Manager the first time Kinoklip starts). Written from general
knowledge of how Kdenlive is used; nothing is copied from its documentation.

Kdenlive's default keys (it uses the KDE standard keys for the file, edit and settings
commands). Shift+R cuts the clip at the playhead; S and X pick the Selection and Razor
tools; V and B insert and overwrite the clip zone.

**Confidence** says how sure we are that the key is that editor's default:
- **sure**: well-known default, used daily;
- **likely**: believed correct, not checked against the program;
- **unsure**: best guess; please correct it in `scripts/keymap-presets.py` if it differs.

Counts: 362 commands; 41 mapped to Kdenlive's key (sure 29, likely 11,
unsure 1); 143 keep the default (Resolve) key; 5 have no key because
Kdenlive uses it for something else; 173 have no key in either.

To change a key: edit the preset's section in `scripts/keymap-presets.py` and run it (it writes
the JSON file and this page and stops when a key is used twice in one scope);
`test_keypresets` checks every preset the same way.

## Mapped to Kdenlive

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| New Project | `file.new` | `Ctrl+N` | sure |  |
| Open Project (file dialog) | `file.open` | `Ctrl+O` | sure |  |
| Save Project | `file.save` | `Ctrl+S` | sure |  |
| Save Project As | `file.save_as` | `Ctrl+Shift+S` | sure |  |
| Quit | `file.quit` | `Ctrl+Q` | likely |  |
| Undo | `edit.undo` | `Ctrl+Z` | sure |  |
| Redo | `edit.redo` | `Ctrl+Shift+Z` | sure |  |
| Cut | `edit.cut` | `Ctrl+X` | sure |  |
| Copy | `edit.copy` | `Ctrl+C` | sure |  |
| Paste | `edit.paste` | `Ctrl+V` | sure |  |
| Delete Selected (lift, leaves a gap) | `edit.delete` | `Del` | sure | Delete Selected Item |
| Select All | `edit.select_all` | `Ctrl+A` | likely |  |
| Insert Clip | `edit.insert` | `V` | likely | Insert Clip Zone |
| Overwrite Clip | `edit.overwrite` | `B` | likely | Overwrite Clip Zone |
| Keyboard Customization | `edit.keyboard_customization` | `Ctrl+Alt+,` | likely | KDE's Configure Keyboard Shortcuts |
| Preferences | `edit.preferences` | `Ctrl+Shift+,` | sure | KDE's Configure |
| Selection Mode | `tool.select` | `S` | sure |  |
| Blade Edit Mode | `tool.blade` | `X` | sure | Razor tool |
| Split Clip (Razor) at Playhead | `edit.split` | `Shift+R` | sure | Cut Clip at the playhead |
| Zoom In | `timeline.zoom_in` | `Ctrl+=` | sure |  |
| Zoom Out | `timeline.zoom_out` | `Ctrl+-` | sure |  |
| Zoom to Fit | `timeline.zoom_fit` | `Ctrl+0` / `Shift+Z` | unsure | Fit Zoom to Project; Shift+Z kept |
| Link / Unlink Clips | `clip.link` | `Ctrl+G` | sure | Group Clips |
| Unlink Clips | `clip.unlink` | `Ctrl+Shift+G` | sure | Ungroup Clips |
| Mark In | `mark.in` | `I` | sure |  |
| Mark Out | `mark.out` | `O` | sure |  |
| Deliver Page | `view.page_deliver` | `Ctrl+Return` | likely | Render |
| Full Screen Window | `view.fullscreen` | `Ctrl+Shift+F` | sure | KDE's Full Screen Mode |
| Play / Stop | `transport.play_pause` | `Space` | sure |  |
| Shuttle Reverse (J, again = faster) | `transport.shuttle_reverse` | `J` | sure |  |
| Stop (K; K+J / K+L step and slow play) | `transport.shuttle_stop` | `K` | sure |  |
| Shuttle Forward (L, again = faster) | `transport.shuttle_forward` | `L` | sure |  |
| Previous Frame | `transport.prev_frame` | `Left` | sure |  |
| Next Frame | `transport.next_frame` | `Right` | sure |  |
| Back One Second | `transport.back_1s` | `Shift+Left` | likely | one second |
| Forward One Second | `transport.forward_1s` | `Shift+Right` | likely |  |
| Previous Edit | `transport.prev_edit` | `Alt+Left` / `Up` | likely | previous snap point; Up kept |
| Next Edit | `transport.next_edit` | `Alt+Right` / `Down` | likely | next snap point; Down kept |
| Go to Timeline Start | `transport.go_to_start` | `Home` | sure |  |
| Go to Timeline End | `transport.go_to_end` | `End` | sure |  |
| Play In to Out | `transport.play_in_out` | `Ctrl+Space` | likely | Play Zone |

## Unbound: the key does something else in Kdenlive

Rebind these in Keyboard Customization if you use them.

| Command | Action id | Default key | Why |
|---|---|---|---|
| New Timeline | `timeline.new` | `Ctrl+N` | Ctrl+N is New Project here |
| Select Nearest Edit Point | `trim.select_nearest_edit` | `V` | V is Insert Clip here |
| Swap Clip with Previous | `trim.swap_left` | `Ctrl+Shift+,` | Ctrl+Shift+, is Preferences here |
| Freeze Frame (the clip shows the frame at the playhead) | `clip.freeze_frame` | `Shift+R` | Shift+R is Split Clip here |
| Mark Clip | `mark.clip` | `X` | X is Blade Edit Mode here |

## Keeping the default (Resolve) key

No Kdenlive counterpart is known, so these keep Kinoklip's default key (a key it
shares with a Kdenlive command was removed, see the note).

| Command | Action id | Key | Note |
|---|---|---|---|
| Project Manager | `file.project_manager` | `Shift+1` |  |
| Import Media | `file.import_media` | `Ctrl+I` |  |
| Project Settings | `file.project_settings` | `Shift+9` |  |
| Paste Insert | `edit.paste_insert` | `Ctrl+Shift+V` |  |
| Paste Attributes | `edit.paste_attributes` | `Alt+V` |  |
| Ripple Cut | `edit.ripple_cut` | `Ctrl+Shift+X` |  |
| Ripple Delete | `edit.ripple_delete` | `Shift+Backspace` | Del is Delete Selected here |
| Deselect All | `edit.deselect_all` | `Ctrl+Shift+A` |  |
| Select Clips Forward (auto-select tracks) | `edit.select_forward` | `Y` |  |
| Select Clips Backward (auto-select tracks) | `edit.select_backward` | `Ctrl+Y` |  |
| Select Clips Forward on All Tracks | `edit.select_forward_all` | `Alt+Y` |  |
| Select Clips Backward on All Tracks | `edit.select_backward_all` | `Ctrl+Alt+Y` |  |
| Ripple Overwrite | `edit.ripple_overwrite` | `Shift+F10` |  |
| Replace Clip | `edit.replace` | `F11` |  |
| Fit to Fill | `edit.fit_to_fill` | `Shift+F11` |  |
| Place on Top | `edit.place_on_top` | `F12` |  |
| Append at End | `edit.append_at_end` | `Shift+F12` |  |
| Trim Edit Mode | `tool.trim` | `T` |  |
| Dynamic Trim Mode (J/K/L trim the selected edit) | `trim.dynamic` | `W` |  |
| Nudge / Trim One Frame Left | `trim.nudge_left` | `,` |  |
| Nudge / Trim One Frame Right | `trim.nudge_right` | `.` |  |
| Nudge / Trim Several Frames Left | `trim.nudge_left_multi` | `Shift+,` |  |
| Nudge / Trim Several Frames Right | `trim.nudge_right_multi` | `Shift+.` |  |
| Trim Start to Playhead (leaves a gap) | `trim.trim_start` | `Shift+[` |  |
| Trim End to Playhead (leaves a gap) | `trim.trim_end` | `Shift+]` |  |
| Ripple Trim Start to Playhead (closes the gap) | `trim.ripple_trim_start` | `Ctrl+Shift+[` |  |
| Ripple Trim End to Playhead (closes the gap) | `trim.ripple_trim_end` | `Ctrl+Shift+]` |  |
| Extend Edit to Playhead | `trim.extend_edit` | `E` |  |
| Toggle Edit Point Side (out / roll / in) | `trim.cycle_edit` | `U` |  |
| Select Nearest Clip or Gap | `trim.select_nearest_clip` | `Shift+V` |  |
| Swap Clip with Next | `trim.swap_right` | `Ctrl+Shift+.` |  |
| Move Clips Up a Track (video clips of the selection, else its audio) | `trim.move_up` | `Alt+Up` |  |
| Move Clips Down a Track | `trim.move_down` | `Alt+Down` |  |
| Join Clips (remove a through edit) | `edit.join` | `Alt+\` |  |
| Add Default Transition | `timeline.add_transition` | `Ctrl+T` |  |
| Snapping | `timeline.snapping` | `N` |  |
| Linked Selection | `timeline.linked_selection` | `Ctrl+Shift+L` |  |
| Audio Scrubbing (a short sound while the playhead is dragged or stepped) | `timeline.audio_scrubbing` | `Shift+S` |  |
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
| Enable / Disable Clip | `clip.toggle_enabled` | `D` |  |
| Match Frame (load the source at this frame) | `clip.match_frame` | `F` |  |
| Change Clip Duration | `clip.change_duration` | `Ctrl+D` |  |
| Change Clip Speed | `clip.change_speed` | `R` |  |
| Retime Controls (speed bar on the selected clips) | `clip.retime_controls` | `Ctrl+R` |  |
| Retime Curve (opens the clip's retime curve in the curve editor) | `clip.retime_curve` | `Ctrl+Shift+R` |  |
| Add Keyframe (inspector) | `clip.add_keyframe` | `Alt+K` |  |
| Previous Keyframe | `clip.prev_keyframe` | `[` |  |
| Next Keyframe | `clip.next_keyframe` | `]` |  |
| Mark Selection | `mark.selection` | `Shift+A` |  |
| Clear In | `mark.clear_in` | `Alt+I` |  |
| Clear Out | `mark.clear_out` | `Alt+O` |  |
| Clear In and Out | `mark.clear_both` | `Alt+X` |  |
| Go to In | `mark.go_to_in` | `Shift+I` |  |
| Go to Out | `mark.go_to_out` | `Shift+O` |  |
| Add Marker (on the selected clip: clip marker) | `mark.add_marker` | `M` |  |
| Add and Modify Marker | `mark.modify_marker` | `Ctrl+M` |  |
| Delete Marker | `mark.delete_marker` | `Alt+M` |  |
| Media Page | `view.page_media` | `Shift+2` |  |
| Cut Page | `view.page_cut` | `Shift+3` |  |
| Edit Page | `view.page_edit` | `Shift+4` |  |
| Compositing Page (node compositing of the selected clip) | `view.page_compositing` | `Shift+5` |  |
| Color Page (grade the clip under the playhead) | `view.page_color` | `Shift+6` |  |
| Audio Page | `view.page_audio` | `Shift+7` |  |
| Toggle Source / Timeline Viewer | `view.toggle_source_timeline` | `Q` |  |
| Viewer Full Screen | `view.fullscreen_viewer` | `Ctrl+F` |  |
| Viewer Zoom to Fit (the viewer under the pointer, else the one with focus, else all) | `view.viewer_zoom_fit` | `Z` |  |
| Play Reverse | `transport.play_reverse` | `Shift+Space` |  |
| Record Voice-Over (count-in, then record the armed audio track while the timeline plays) | `transport.record` | `Ctrl+Alt+R` |  |
| Previous Marker | `transport.prev_marker` | `Shift+Up` |  |
| Next Marker | `transport.next_marker` | `Shift+Down` |  |
| Loop | `transport.loop` | `Ctrl+/` |  |
| Play Around Current Frame | `transport.play_around` | `/` |  |
| New Bin | `mediapool.new_bin` | `Ctrl+Shift+N` |  |
| Video Scopes (floating window: waveform, parade, vectorscope, histogram) | `view.video_scopes` | `Ctrl+Shift+W` |  |
| Find in Media Pool (timeline clip) | `timeline.find_in_media_pool` | `Alt+F` |  |
| Cut / Switch to Multicam Angle 1 | `multicam.angle_1` | `1` |  |
| Cut / Switch to Multicam Angle 2 | `multicam.angle_2` | `2` |  |
| Cut / Switch to Multicam Angle 3 | `multicam.angle_3` | `3` |  |
| Cut / Switch to Multicam Angle 4 | `multicam.angle_4` | `4` |  |
| Cut / Switch to Multicam Angle 5 | `multicam.angle_5` | `5` |  |
| Cut / Switch to Multicam Angle 6 | `multicam.angle_6` | `6` |  |
| Cut / Switch to Multicam Angle 7 | `multicam.angle_7` | `7` |  |
| Cut / Switch to Multicam Angle 8 | `multicam.angle_8` | `8` |  |
| Cut / Switch to Multicam Angle 9 | `multicam.angle_9` | `9` |  |
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
| Fit Nodes to View | `comp.fit` | `Shift+F` |  |
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

## Kdenlive commands Kinoklip does not have (gaps)

- Spacer (M), Slip, Ripple and Multicam tools; track selection with Up / Down (here they go to the previous / next edit, as in Resolve).
- Insert Space, Remove Space, Extract / Lift Zone keys, Switch Monitor (T), Add Clip Marker with a comment.
- Kdenlive's Ripple Delete has no default key; Shift+Backspace stays.
- Previous / next marker and trim to the playhead: no default keys known.
