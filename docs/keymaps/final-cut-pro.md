# Keymap: Final Cut Pro (Ctrl for Command)

A built-in keyboard preset (`resources/keymaps/final-cut-pro.json`, profile id `final-cut-pro`) for people
coming from Final Cut Pro. Choose it in Kinoklip menu > Keyboard Customization > Keyboard
profile (or in the Project Manager the first time Kinoklip starts). Written from general
knowledge of how Final Cut Pro is used; nothing is copied from its documentation.

Final Cut Pro's default keys, moved to a PC keyboard: **Command becomes Ctrl**, Option
becomes Alt, and Control becomes Meta (the Super / Windows key, which some desktops keep for
themselves: rebind those few in Keyboard Customization if they do nothing). Final Cut's
magnetic timeline closes gaps by itself, so Delete is Ripple Delete here and Shift+Delete
leaves a gap.

**Confidence** says how sure we are that the key is that editor's default:
- **sure**: well-known default, used daily;
- **likely**: believed correct, not checked against the program;
- **unsure**: best guess; please correct it in `scripts/keymap-presets.py` if it differs.

Counts: 333 commands; 88 mapped to Final Cut Pro's key (sure 75, likely 12,
unsure 1); 97 keep the default (Resolve) key; 9 have no key because
Final Cut Pro uses it for something else; 139 have no key in either.

To change a key: edit the preset's section in `scripts/keymap-presets.py` and run it (it writes
the JSON file and this page and stops when a key is used twice in one scope);
`test_keypresets` checks every preset the same way.

## Mapped to Final Cut Pro

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| New Timeline | `timeline.new` | `Ctrl+N` | sure | New Project (a timeline in Final Cut) |
| Open Project (file dialog) | `file.open` | `Ctrl+O` | sure |  |
| Save Project | `file.save` | `Ctrl+S` | sure |  |
| Import Media | `file.import_media` | `Ctrl+I` | sure |  |
| Quit | `file.quit` | `Ctrl+Q` | likely |  |
| Undo | `edit.undo` | `Ctrl+Z` | sure |  |
| Redo | `edit.redo` | `Ctrl+Shift+Z` | sure |  |
| Cut | `edit.cut` | `Ctrl+X` | sure |  |
| Copy | `edit.copy` | `Ctrl+C` | sure |  |
| Paste | `edit.paste` | `Ctrl+V` | sure |  |
| Paste Attributes | `edit.paste_attributes` | `Ctrl+Shift+V` | sure |  |
| Delete Selected (lift, leaves a gap) | `edit.delete` | `Shift+Backspace` | sure | Replace with Gap |
| Ripple Delete | `edit.ripple_delete` | `Backspace` / `Del` | sure | Delete in the magnetic timeline closes the gap |
| Select All | `edit.select_all` | `Ctrl+A` | sure |  |
| Deselect All | `edit.deselect_all` | `Ctrl+Shift+A` | sure |  |
| Insert Clip | `edit.insert` | `W` | sure |  |
| Overwrite Clip | `edit.overwrite` | `D` | sure |  |
| Replace Clip | `edit.replace` | `Shift+R` | sure |  |
| Place on Top | `edit.place_on_top` | `Q` | likely | Connect; the clip goes on the track above |
| Append at End | `edit.append_at_end` | `E` | sure |  |
| Keyboard Customization | `edit.keyboard_customization` | `Ctrl+Alt+K` | sure | Command Editor |
| Preferences | `edit.preferences` | `Ctrl+,` | sure |  |
| Selection Mode | `tool.select` | `A` | sure |  |
| Trim Edit Mode | `tool.trim` | `T` | sure |  |
| Blade Edit Mode | `tool.blade` | `B` | sure |  |
| Nudge / Trim One Frame Left | `trim.nudge_left` | `,` | sure |  |
| Nudge / Trim One Frame Right | `trim.nudge_right` | `.` | sure |  |
| Nudge / Trim Several Frames Left | `trim.nudge_left_multi` | `Shift+,` | sure |  |
| Nudge / Trim Several Frames Right | `trim.nudge_right_multi` | `Shift+.` | sure |  |
| Trim Start to Playhead (leaves a gap) | `trim.trim_start` | `Alt+[` | sure |  |
| Trim End to Playhead (leaves a gap) | `trim.trim_end` | `Alt+]` | sure |  |
| Extend Edit to Playhead | `trim.extend_edit` | `Shift+X` | sure |  |
| Split Clip (Razor) at Playhead | `edit.split` | `Ctrl+B` | sure | Blade at the playhead |
| Add Default Transition | `timeline.add_transition` | `Ctrl+T` | sure |  |
| Snapping | `timeline.snapping` | `N` | sure |  |
| Audio Scrubbing (a short sound while the playhead is dragged or stepped) | `timeline.audio_scrubbing` | `Shift+S` | sure | Audio Skimming |
| Zoom In | `timeline.zoom_in` | `Ctrl+=` | sure |  |
| Zoom Out | `timeline.zoom_out` | `Ctrl+-` | sure |  |
| Zoom to Fit | `timeline.zoom_fit` | `Shift+Z` | sure |  |
| Timeline Settings | `timeline.settings` | `Ctrl+J` | likely | Project Properties |
| Enable / Disable Clip | `clip.toggle_enabled` | `V` | sure |  |
| Flag Clip (toggles the flag colour picked in the tool bar's flag menu on the selected clips' media) | `clip.flag` | `F` | unsure | Favourite in Final Cut; Kinoklip flags the clip |
| Match Frame (load the source at this frame) | `clip.match_frame` | `Shift+F` | sure | Reveal in Browser |
| Change Clip Duration | `clip.change_duration` | `Meta+D` | sure | Control+D |
| Retime Controls (speed bar on the selected clips) | `clip.retime_controls` | `Ctrl+R` | sure | Retime Editor |
| Freeze Frame (the clip shows the frame at the playhead) | `clip.freeze_frame` | `Alt+F` | sure | Add Freeze Frame |
| New Compound Clip | `clip.new_compound` | `Alt+G` | sure |  |
| Decompose in Place (compound clip) | `clip.decompose` | `Ctrl+Shift+G` | sure | Break Apart Clip Items |
| Unlink Clips | `clip.unlink` | `Meta+Shift+S` | likely | Detach Audio (Control+Shift+S) |
| Add Keyframe (inspector) | `clip.add_keyframe` | `Alt+K` | sure |  |
| Previous Keyframe | `clip.prev_keyframe` | `Alt+;` | sure |  |
| Next Keyframe | `clip.next_keyframe` | `Alt+'` | sure |  |
| Mark In | `mark.in` | `I` | sure |  |
| Mark Out | `mark.out` | `O` | sure |  |
| Mark Clip | `mark.clip` | `X` | sure |  |
| Clear In | `mark.clear_in` | `Alt+I` | sure |  |
| Clear Out | `mark.clear_out` | `Alt+O` | sure |  |
| Clear In and Out | `mark.clear_both` | `Alt+X` | sure |  |
| Go to In | `mark.go_to_in` | `Shift+I` | sure |  |
| Go to Out | `mark.go_to_out` | `Shift+O` | sure |  |
| Add Marker (on the selected clip: clip marker) | `mark.add_marker` | `M` | sure |  |
| Add and Modify Marker | `mark.modify_marker` | `Alt+M` | likely | Add Marker and Modify |
| Delete Marker | `mark.delete_marker` | `Meta+M` | likely | Control+M |
| Deliver Page | `view.page_deliver` | `Ctrl+E` | likely | Share (Export File); Kinoklip's is the Deliver page |
| Media Pool panel | `view.toggle_media_pool` | `Ctrl+1` | likely | Browser |
| Effects panel | `view.toggle_effects` | `Ctrl+5` | sure | Effects Browser |
| Inspector panel | `view.toggle_inspector` | `Ctrl+4` | sure | Inspector |
| Viewer Full Screen | `view.fullscreen_viewer` | `Ctrl+Shift+F` | sure | Play Full Screen |
| Full Screen Window | `view.fullscreen` | `Ctrl+Meta+F` | likely | Enter Full Screen (Control+Command+F) |
| Play / Stop | `transport.play_pause` | `Space` | sure |  |
| Shuttle Reverse (J, again = faster) | `transport.shuttle_reverse` | `J` | sure |  |
| Stop (K; K+J / K+L step and slow play) | `transport.shuttle_stop` | `K` | sure |  |
| Shuttle Forward (L, again = faster) | `transport.shuttle_forward` | `L` | sure |  |
| Previous Frame | `transport.prev_frame` | `Left` | sure |  |
| Next Frame | `transport.next_frame` | `Right` | sure |  |
| Back One Second | `transport.back_1s` | `Shift+Left` | likely | 10 frames in Final Cut |
| Forward One Second | `transport.forward_1s` | `Shift+Right` | likely | 10 frames in Final Cut |
| Previous Edit | `transport.prev_edit` | `Up` | sure |  |
| Next Edit | `transport.next_edit` | `Down` | sure |  |
| Go to Timeline Start | `transport.go_to_start` | `Home` | sure |  |
| Go to Timeline End | `transport.go_to_end` | `End` | sure |  |
| Previous Marker | `transport.prev_marker` | `Meta+;` | sure | Control+; |
| Next Marker | `transport.next_marker` | `Meta+'` | sure | Control+' |
| Loop | `transport.loop` | `Ctrl+L` | sure |  |
| Play In to Out | `transport.play_in_out` | `/` | sure | Play Selection |
| Play Around Current Frame | `transport.play_around` | `Shift+/` | sure | Play Around (Shift+?) |
| New Bin | `mediapool.new_bin` | `Ctrl+Shift+N` | likely | New Folder |
| Video Scopes (floating window: waveform, parade, vectorscope, histogram) | `view.video_scopes` | `Ctrl+7` | sure | Video Scopes |

## Unbound: the key does something else in Final Cut Pro

Rebind these in Keyboard Customization if you use them.

| Command | Action id | Default key | Why |
|---|---|---|---|
| Paste Insert | `edit.paste_insert` | `Ctrl+Shift+V` | Ctrl+Shift+V is Paste Attributes here |
| Dynamic Trim Mode (J/K/L trim the selected edit) | `trim.dynamic` | `W` | W is Insert Clip here |
| Select Nearest Edit Point | `trim.select_nearest_edit` | `V` | V is Enable / Disable Clip here |
| Toggle Auto Select V1 | `timeline.auto_select_video_1` | `Ctrl+1` | Ctrl+1 is Media Pool panel here |
| Toggle Auto Select V4 | `timeline.auto_select_video_4` | `Ctrl+4` | Ctrl+4 is Inspector panel here |
| Toggle Auto Select V5 | `timeline.auto_select_video_5` | `Ctrl+5` | Ctrl+5 is Effects panel here |
| Toggle Auto Select V7 | `timeline.auto_select_video_7` | `Ctrl+7` | Ctrl+7 is Video Scopes here |
| Toggle Source / Timeline Viewer | `view.toggle_source_timeline` | `Q` | Q is Place on Top here |
| Find in Media Pool (timeline clip) | `timeline.find_in_media_pool` | `Alt+F` | Alt+F is Freeze Frame here |

## Keeping the default (Resolve) key

No Final Cut Pro counterpart is known, so these keep Kinoklip's default key (a key it
shares with a Final Cut Pro command was removed, see the note).

| Command | Action id | Key | Note |
|---|---|---|---|
| Project Manager | `file.project_manager` | `Shift+1` |  |
| Save Project As | `file.save_as` | `Ctrl+Shift+S` |  |
| Project Settings | `file.project_settings` | `Shift+9` |  |
| Ripple Cut | `edit.ripple_cut` | `Ctrl+Shift+X` |  |
| Select Clips Forward (auto-select tracks) | `edit.select_forward` | `Y` |  |
| Select Clips Backward (auto-select tracks) | `edit.select_backward` | `Ctrl+Y` |  |
| Select Clips Forward on All Tracks | `edit.select_forward_all` | `Alt+Y` |  |
| Select Clips Backward on All Tracks | `edit.select_backward_all` | `Ctrl+Alt+Y` |  |
| Ripple Overwrite | `edit.ripple_overwrite` | `Shift+F10` |  |
| Fit to Fill | `edit.fit_to_fill` | `Shift+F11` |  |
| Ripple Trim Start to Playhead (closes the gap) | `trim.ripple_trim_start` | `Ctrl+Shift+[` |  |
| Ripple Trim End to Playhead (closes the gap) | `trim.ripple_trim_end` | `Ctrl+Shift+]` |  |
| Toggle Edit Point Side (out / roll / in) | `trim.cycle_edit` | `U` |  |
| Select Nearest Clip or Gap | `trim.select_nearest_clip` | `Shift+V` |  |
| Swap Clip with Previous | `trim.swap_left` | `Ctrl+Shift+,` |  |
| Swap Clip with Next | `trim.swap_right` | `Ctrl+Shift+.` |  |
| Move Clips Up a Track (video clips of the selection, else its audio) | `trim.move_up` | `Alt+Up` |  |
| Move Clips Down a Track | `trim.move_down` | `Alt+Down` |  |
| Join Clips (remove a through edit) | `edit.join` | `Alt+\` |  |
| Linked Selection | `timeline.linked_selection` | `Ctrl+Shift+L` |  |
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
| Toggle Auto Select V2 | `timeline.auto_select_video_2` | `Ctrl+2` |  |
| Toggle Auto Select V3 | `timeline.auto_select_video_3` | `Ctrl+3` |  |
| Toggle Auto Select V6 | `timeline.auto_select_video_6` | `Ctrl+6` |  |
| Toggle Auto Select V8 | `timeline.auto_select_video_8` | `Ctrl+8` |  |
| Toggle Auto Select A1 | `timeline.auto_select_audio_1` | `Ctrl+Shift+1` |  |
| Toggle Auto Select A2 | `timeline.auto_select_audio_2` | `Ctrl+Shift+2` |  |
| Toggle Auto Select A3 | `timeline.auto_select_audio_3` | `Ctrl+Shift+3` |  |
| Toggle Auto Select A4 | `timeline.auto_select_audio_4` | `Ctrl+Shift+4` |  |
| Toggle Auto Select A5 | `timeline.auto_select_audio_5` | `Ctrl+Shift+5` |  |
| Toggle Auto Select A6 | `timeline.auto_select_audio_6` | `Ctrl+Shift+6` |  |
| Toggle Auto Select A7 | `timeline.auto_select_audio_7` | `Ctrl+Shift+7` |  |
| Toggle Auto Select A8 | `timeline.auto_select_audio_8` | `Ctrl+Shift+8` |  |
| Change Clip Speed | `clip.change_speed` | `R` |  |
| Retime Curve (opens the clip's retime curve in the curve editor) | `clip.retime_curve` | `Ctrl+Shift+R` |  |
| Link / Unlink Clips | `clip.link` | `Ctrl+Alt+L` |  |
| Mark Selection | `mark.selection` | `Shift+A` |  |
| Media Page | `view.page_media` | `Shift+2` |  |
| Cut Page | `view.page_cut` | `Shift+3` |  |
| Edit Page | `view.page_edit` | `Shift+4` |  |
| Compositing Page (node compositing of the selected clip) | `view.page_compositing` | `Shift+5` |  |
| Color Page (grade the clip under the playhead) | `view.page_color` | `Shift+6` |  |
| Audio Page | `view.page_audio` | `Shift+7` |  |
| Viewer Zoom to Fit (the viewer under the pointer, else the one with focus, else all) | `view.viewer_zoom_fit` | `Z` |  |
| Play Reverse | `transport.play_reverse` | `Shift+Space` |  |
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

## Final Cut Pro commands Kinoklip does not have (gaps)

- Connect (Q) puts the clip on the track above (Place on Top); there are no connected clips or storylines (G).
- Position (P) and Range (R) tools, Skimming (S), Solo (Alt+S), Audition (Y).
- Lift from Storyline (Ctrl+Alt+Up), Overwrite to Primary (Ctrl+Alt+Down).
- Final Cut saves by itself; Ctrl+S saves here as usual.
- Shift+Left / Right step 10 frames in Final Cut, 1 second in Kinoklip.
