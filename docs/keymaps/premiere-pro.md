# Keymap: Premiere Pro (Windows)

A built-in keyboard preset (`resources/keymaps/premiere-pro.json`, profile id `premiere-pro`) for people
coming from Premiere Pro. Choose it in Kinoklip menu > Keyboard Customization > Keyboard
profile (or in the Project Manager the first time Kinoklip starts). Written from general
knowledge of how Premiere Pro is used; nothing is copied from its documentation.

Premiere Pro's default Windows keys. Premiere's panel keys (Shift+1 ... Shift+9) show
Kinoklip's matching panels; its workspaces (Alt+Shift+1 ...) switch Kinoklip's pages.

**Confidence** says how sure we are that the key is that editor's default:
- **sure**: well-known default, used daily;
- **likely**: believed correct, not checked against the program;
- **unsure**: best guess; please correct it in `scripts/keymap-presets.py` if it differs.

Counts: 338 commands; 87 mapped to Premiere Pro's key (sure 58, likely 22,
unsure 7); 97 keep the default (Resolve) key; 7 have no key because
Premiere Pro uses it for something else; 147 have no key in either.

To change a key: edit the preset's section in `scripts/keymap-presets.py` and run it (it writes
the JSON file and this page and stops when a key is used twice in one scope);
`test_keypresets` checks every preset the same way.

## Mapped to Premiere Pro

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| New Project | `file.new` | `Ctrl+Alt+N` | sure | New Project |
| New Timeline | `timeline.new` | `Ctrl+N` | sure | New Sequence |
| Open Project (file dialog) | `file.open` | `Ctrl+O` | sure |  |
| Save Project | `file.save` | `Ctrl+S` | sure |  |
| Save Project As | `file.save_as` | `Ctrl+Shift+S` | sure |  |
| Import Media | `file.import_media` | `Ctrl+I` | sure |  |
| Quit | `file.quit` | `Ctrl+Q` | likely |  |
| Undo | `edit.undo` | `Ctrl+Z` | sure |  |
| Redo | `edit.redo` | `Ctrl+Shift+Z` | sure |  |
| Cut | `edit.cut` | `Ctrl+X` | sure |  |
| Copy | `edit.copy` | `Ctrl+C` | sure |  |
| Paste | `edit.paste` | `Ctrl+V` | sure |  |
| Paste Insert | `edit.paste_insert` | `Ctrl+Shift+V` | sure |  |
| Paste Attributes | `edit.paste_attributes` | `Ctrl+Alt+V` | sure |  |
| Delete Selected (lift, leaves a gap) | `edit.delete` | `Del` / `Backspace` | sure | Clear |
| Ripple Delete | `edit.ripple_delete` | `Shift+Del` | likely |  |
| Select All | `edit.select_all` | `Ctrl+A` | likely |  |
| Deselect All | `edit.deselect_all` | `Ctrl+Shift+A` | sure |  |
| Select Clips Forward (auto-select tracks) | `edit.select_forward` | `A` | likely | Track Select Forward tool |
| Select Clips Backward (auto-select tracks) | `edit.select_backward` | `Shift+A` | likely | Track Select Backward tool |
| Insert Clip | `edit.insert` | `,` | sure |  |
| Overwrite Clip | `edit.overwrite` | `.` | sure |  |
| Keyboard Customization | `edit.keyboard_customization` | `Ctrl+Alt+K` | sure |  |
| Selection Mode | `tool.select` | `V` | sure |  |
| Trim Edit Mode | `tool.trim` | `B` | likely | Ripple Edit tool; Kinoklip's Trim tool also rolls, slips and slides |
| Blade Edit Mode | `tool.blade` | `C` | sure | Razor tool |
| Nudge / Trim One Frame Left | `trim.nudge_left` | `Alt+Left` | sure | Nudge Clip Selection Left (one frame) |
| Nudge / Trim One Frame Right | `trim.nudge_right` | `Alt+Right` | sure |  |
| Nudge / Trim Several Frames Left | `trim.nudge_left_multi` | `Alt+Shift+Left` | likely | five frames |
| Nudge / Trim Several Frames Right | `trim.nudge_right_multi` | `Alt+Shift+Right` | likely |  |
| Trim Start to Playhead (leaves a gap) | `trim.trim_start` | `Ctrl+Alt+Q` | likely | Trim Previous Edit to Playhead |
| Trim End to Playhead (leaves a gap) | `trim.trim_end` | `Ctrl+Alt+W` | likely | Trim Next Edit to Playhead |
| Ripple Trim Start to Playhead (closes the gap) | `trim.ripple_trim_start` | `Q` | sure | Ripple Trim Previous Edit to Playhead |
| Ripple Trim End to Playhead (closes the gap) | `trim.ripple_trim_end` | `W` | sure | Ripple Trim Next Edit to Playhead |
| Extend Edit to Playhead | `trim.extend_edit` | `E` | sure |  |
| Select Nearest Edit Point | `trim.select_nearest_edit` | `T` | likely | Select Nearest Edit Point |
| Select Nearest Clip or Gap | `trim.select_nearest_clip` | `D` | likely | Select Clip at Playhead |
| Split Clip (Razor) at Playhead | `edit.split` | `Ctrl+K` | sure | Add Edit |
| Add Default Transition | `timeline.add_transition` | `Ctrl+D` | sure | Apply Video Transition |
| Snapping | `timeline.snapping` | `S` | sure |  |
| Audio Scrubbing (a short sound while the playhead is dragged or stepped) | `timeline.audio_scrubbing` | `Shift+S` | unsure | audio while scrubbing; not sure Premiere has a default key |
| Zoom In | `timeline.zoom_in` | `=` | sure |  |
| Zoom Out | `timeline.zoom_out` | `-` | sure |  |
| Zoom to Fit | `timeline.zoom_fit` | `\` | sure | Zoom to Sequence |
| Enable / Disable Clip | `clip.toggle_enabled` | `Shift+E` | sure |  |
| Match Frame (load the source at this frame) | `clip.match_frame` | `F` | sure |  |
| Change Clip Speed | `clip.change_speed` | `Ctrl+R` | sure | Speed / Duration |
| Link / Unlink Clips | `clip.link` | `Ctrl+L` | sure | Link / Unlink toggle |
| Mark In | `mark.in` | `I` | sure |  |
| Mark Out | `mark.out` | `O` | sure |  |
| Mark Clip | `mark.clip` | `X` | sure |  |
| Mark Selection | `mark.selection` | `/` | sure |  |
| Clear In | `mark.clear_in` | `Ctrl+Shift+I` | sure |  |
| Clear Out | `mark.clear_out` | `Ctrl+Shift+O` | sure |  |
| Clear In and Out | `mark.clear_both` | `Ctrl+Shift+X` | sure |  |
| Go to In | `mark.go_to_in` | `Shift+I` | sure |  |
| Go to Out | `mark.go_to_out` | `Shift+O` | sure |  |
| Add Marker (on the selected clip: clip marker) | `mark.add_marker` | `M` | sure |  |
| Media Page | `view.page_media` | `Alt+Shift+1` | unsure | Premiere's workspaces 1-6 map to Kinoklip's pages |
| Cut Page | `view.page_cut` | `Alt+Shift+2` | unsure |  |
| Edit Page | `view.page_edit` | `Alt+Shift+3` | unsure |  |
| Compositing Page (node compositing of the selected clip) | `view.page_compositing` | `Alt+Shift+4` | unsure |  |
| Color Page (grade the clip under the playhead) | `view.page_color` | `Alt+Shift+5` | unsure |  |
| Audio Page | `view.page_audio` | `Alt+Shift+6` | unsure |  |
| Deliver Page | `view.page_deliver` | `Ctrl+M` | likely | Export Media opens Premiere's export; Kinoklip's is the Deliver page |
| Media Pool panel | `view.toggle_media_pool` | `Shift+1` | likely | Project panel |
| Effects panel | `view.toggle_effects` | `Shift+7` | likely | Effects panel |
| Mixer panel | `view.toggle_mixer` | `Shift+6` | likely | Audio Track Mixer |
| Inspector panel | `view.toggle_inspector` | `Shift+5` | likely | Effect Controls panel |
| Viewer Full Screen | `view.fullscreen_viewer` | `Ctrl+`` | likely | Toggle Full Screen |
| Play / Stop | `transport.play_pause` | `Space` | sure |  |
| Shuttle Reverse (J, again = faster) | `transport.shuttle_reverse` | `J` | sure |  |
| Stop (K; K+J / K+L step and slow play) | `transport.shuttle_stop` | `K` | sure |  |
| Shuttle Forward (L, again = faster) | `transport.shuttle_forward` | `L` | sure |  |
| Previous Frame | `transport.prev_frame` | `Left` | sure |  |
| Next Frame | `transport.next_frame` | `Right` | sure |  |
| Back One Second | `transport.back_1s` | `Shift+Left` | likely | 5 frames in Premiere |
| Forward One Second | `transport.forward_1s` | `Shift+Right` | likely | 5 frames in Premiere |
| Previous Edit | `transport.prev_edit` | `Up` / `PgUp` | sure |  |
| Next Edit | `transport.next_edit` | `Down` / `PgDown` | sure |  |
| Go to Timeline Start | `transport.go_to_start` | `Home` | sure |  |
| Go to Timeline End | `transport.go_to_end` | `End` | sure |  |
| Previous Marker | `transport.prev_marker` | `Ctrl+Shift+M` | sure |  |
| Next Marker | `transport.next_marker` | `Shift+M` | sure |  |
| Play In to Out | `transport.play_in_out` | `Ctrl+Shift+Space` | likely |  |
| Play Around Current Frame | `transport.play_around` | `Shift+K` | likely |  |
| New Bin | `mediapool.new_bin` | `Ctrl+B` | sure |  |

## Unbound: the key does something else in Premiere Pro

Rebind these in Keyboard Customization if you use them.

| Command | Action id | Default key | Why |
|---|---|---|---|
| Project Manager | `file.project_manager` | `Shift+1` | Shift+1 is Media Pool panel here |
| Ripple Cut | `edit.ripple_cut` | `Ctrl+Shift+X` | Ctrl+Shift+X is Clear In and Out here |
| Dynamic Trim Mode (J/K/L trim the selected edit) | `trim.dynamic` | `W` | W is Ripple Trim End to Playhead here |
| Change Clip Duration | `clip.change_duration` | `Ctrl+D` | Ctrl+D is Add Default Transition here |
| Retime Controls (speed bar on the selected clips) | `clip.retime_controls` | `Ctrl+R` | Ctrl+R is Change Clip Speed here |
| Add and Modify Marker | `mark.modify_marker` | `Ctrl+M` | Ctrl+M is Deliver Page here |
| Toggle Source / Timeline Viewer | `view.toggle_source_timeline` | `Q` | Q is Ripple Trim Start to Playhead here |

## Keeping the default (Resolve) key

No Premiere Pro counterpart is known, so these keep Kinoklip's default key (a key it
shares with a Premiere Pro command was removed, see the note).

| Command | Action id | Key | Note |
|---|---|---|---|
| Project Settings | `file.project_settings` | `Shift+9` |  |
| Select Clips Forward on All Tracks | `edit.select_forward_all` | `Alt+Y` |  |
| Select Clips Backward on All Tracks | `edit.select_backward_all` | `Ctrl+Alt+Y` |  |
| Ripple Overwrite | `edit.ripple_overwrite` | `Shift+F10` |  |
| Replace Clip | `edit.replace` | `F11` |  |
| Fit to Fill | `edit.fit_to_fill` | `Shift+F11` |  |
| Place on Top | `edit.place_on_top` | `F12` |  |
| Append at End | `edit.append_at_end` | `Shift+F12` |  |
| Preferences | `edit.preferences` | `Ctrl+,` |  |
| Toggle Edit Point Side (out / roll / in) | `trim.cycle_edit` | `U` |  |
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
| Freeze Frame (the clip shows the frame at the playhead) | `clip.freeze_frame` | `Shift+R` |  |
| Retime Curve (opens the clip's retime curve in the curve editor) | `clip.retime_curve` | `Ctrl+Shift+R` |  |
| Add Keyframe (inspector) | `clip.add_keyframe` | `Alt+K` |  |
| Previous Keyframe | `clip.prev_keyframe` | `[` |  |
| Next Keyframe | `clip.next_keyframe` | `]` |  |
| Delete Marker | `mark.delete_marker` | `Alt+M` |  |
| Viewer Zoom to Fit (the viewer under the pointer, else the one with focus, else all) | `view.viewer_zoom_fit` | `Z` |  |
| Full Screen Window | `view.fullscreen` | `Ctrl+Shift+F` |  |
| Play Reverse | `transport.play_reverse` | `Shift+Space` |  |
| Loop | `transport.loop` | `Ctrl+/` |  |
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

## Premiere Pro commands Kinoklip does not have (gaps)

- Slip (Y), Slide (U), Rate Stretch (R), Rolling Edit (N), Pen (P), Hand (H) and Zoom (Z) tools: Kinoklip's Trim tool (B here) does ripple, roll, slip and slide by where you drag.
- Add Edit to All Tracks (Ctrl+Shift+K), Lift (;) and Extract (') of the In / Out range, Nest, Render In to Out (Enter).
- Link / Unlink is one toggle in Premiere (Ctrl+L): here Ctrl+L links (and unlinks a linked group); Unlink has no key.
- Speed / Duration is one dialog in Premiere (Ctrl+R): here Ctrl+R changes the speed; Change Duration has no key.
- Shift+Left / Right step 5 frames in Premiere, 1 second in Kinoklip.
