# Keymap: DaVinci Resolve (Windows)

The built-in default keyboard profile (`resources/keymaps/resolve-windows.json`, profile id
`resolve-windows`). It reproduces the default Windows shortcuts of DaVinci Resolve's Edit page and
its global commands, written from general knowledge of how Resolve behaves (nothing copied from
Resolve's documentation). Commands of subsystems Kinoklip does not have yet (Cut, Fusion, Color and
Fairlight pages, retime curves, freeze frames, audio scrubbing) keep their Resolve key but are
disabled.

**Confidence** says how sure we are that the key is Resolve's default:
- **sure**: well-known default, used daily;
- **likely**: believed correct, not verified against a running Resolve;
- **unsure**: best guess; please correct in the keymap file (and here) if it differs.

Counts: 152 commands; sure 88, likely 39, unsure 25.

Mouse (EditBehaviour, Preferences > Editing): wheel scrolls the tracks vertically, Ctrl+wheel
scrolls the timeline horizontally, Alt+wheel zooms the timeline (around the playhead by default;
"Zoom around: Mouse cursor" anchors it at the pointer), Shift+wheel changes the track height.
Confidence: likely.

To change a default: edit the table in `scripts/keymap-resolve-windows.py` and run it (it writes
the JSON file and this page); `test_actionregistry` checks
that every command has an entry and that no key is used twice.


## File and project

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| New Project | `file.new` | `Ctrl+N` | unsure | Resolve uses Ctrl+N for New Timeline; Kinoklip has one timeline per project, so it creates a new project. |
| Open Project / Project Manager | `file.open` | `Ctrl+O` / `Shift+1` | likely | Shift+1 is Resolve's Project Manager page; Kinoklip has no project manager yet and opens a project instead. Ctrl+O unsure. |
| Save Project | `file.save` | `Ctrl+S` | sure |  |
| Save Project As | `file.save_as` | `Ctrl+Shift+S` | sure |  |
| Import Media | `file.import_media` | `Ctrl+I` | sure |  |
| Project Settings | `file.project_settings` | `Shift+9` | sure |  |
| Quit | `file.quit` | `Ctrl+Q` | sure |  |

## Edit

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Undo | `edit.undo` | `Ctrl+Z` | sure |  |
| Redo | `edit.redo` | `Ctrl+Shift+Z` | sure |  |
| Cut | `edit.cut` | `Ctrl+X` | sure |  |
| Copy | `edit.copy` | `Ctrl+C` | sure |  |
| Paste | `edit.paste` | `Ctrl+V` | sure |  |
| Paste Insert | `edit.paste_insert` | `Ctrl+Shift+V` | sure |  |
| Paste Attributes | `edit.paste_attributes` | `Alt+V` | sure |  |
| Ripple Cut | `edit.ripple_cut` | `Ctrl+Shift+X` | sure |  |
| Delete Selected (lift, leaves a gap) | `edit.delete` | `Backspace` | sure |  |
| Ripple Delete | `edit.ripple_delete` | `Del` / `Shift+Backspace` | sure |  |
| Delete Gaps | `edit.delete_gaps` | (none) | likely | menu command without a default key |
| Select All | `edit.select_all` | `Ctrl+A` | sure |  |
| Deselect All | `edit.deselect_all` | `Ctrl+Shift+A` | sure |  |
| Select Clips Forward (auto-select tracks) | `edit.select_forward` | `Y` | unsure |  |
| Select Clips Backward (auto-select tracks) | `edit.select_backward` | `Ctrl+Y` | unsure |  |
| Select Clips Forward on All Tracks | `edit.select_forward_all` | `Alt+Y` | unsure |  |
| Select Clips Backward on All Tracks | `edit.select_backward_all` | `Ctrl+Alt+Y` | unsure |  |
| Insert Clip | `edit.insert` | `F9` | sure |  |
| Overwrite Clip | `edit.overwrite` | `F10` | sure |  |
| Ripple Overwrite | `edit.ripple_overwrite` | `Shift+F10` | likely |  |
| Replace Clip | `edit.replace` | `F11` | sure |  |
| Fit to Fill | `edit.fit_to_fill` | `Shift+F11` | sure |  |
| Place on Top | `edit.place_on_top` | `F12` | sure |  |
| Append at End | `edit.append_at_end` | `Shift+F12` | sure |  |
| Keyboard Customization | `edit.keyboard_customization` | `Ctrl+Alt+K` | sure |  |
| Preferences | `edit.preferences` | `Ctrl+,` | likely |  |

## Tools and trimming

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Selection Mode | `tool.select` | `A` | sure |  |
| Trim Edit Mode | `tool.trim` | `T` | sure |  |
| Blade Edit Mode | `tool.blade` | `B` | sure |  |
| Dynamic Trim Mode (J/K/L trim the selected edit) | `trim.dynamic` | `W` | sure |  |
| Nudge / Trim One Frame Left | `trim.nudge_left` | `,` | sure |  |
| Nudge / Trim One Frame Right | `trim.nudge_right` | `.` | sure |  |
| Nudge / Trim Several Frames Left | `trim.nudge_left_multi` | `Shift+,` | sure |  |
| Nudge / Trim Several Frames Right | `trim.nudge_right_multi` | `Shift+.` | sure |  |
| Trim Start to Playhead | `trim.trim_start` | `Shift+[` | sure |  |
| Trim End to Playhead | `trim.trim_end` | `Shift+]` | sure |  |
| Extend Edit to Playhead | `trim.extend_edit` | `E` | sure |  |
| Toggle Edit Point Side (out / roll / in) | `trim.cycle_edit` | `U` | sure |  |
| Select Nearest Edit Point | `trim.select_nearest_edit` | `V` | sure |  |
| Select Nearest Clip or Gap | `trim.select_nearest_clip` | `Shift+V` | likely |  |
| Swap Clip with Previous | `trim.swap_left` | `Ctrl+Shift+,` | likely |  |
| Swap Clip with Next | `trim.swap_right` | `Ctrl+Shift+.` | likely |  |

## Timeline

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Split Clip (Razor) at Playhead | `edit.split` | `Ctrl+\` | sure | Ctrl+B is not a default (owner feedback alpha.1 #8). |
| Join Clips (remove a through edit) | `edit.join` | `Alt+\` | unsure |  |
| Add Default Transition | `timeline.add_transition` | `Ctrl+T` | sure |  |
| Snapping | `timeline.snapping` | `N` | sure |  |
| Linked Selection | `timeline.linked_selection` | `Ctrl+Shift+L` | sure |  |
| Audio Scrubbing (disabled: no audio scrubbing yet) | `timeline.audio_scrubbing` | `Shift+S` | sure |  |
| Zoom In | `timeline.zoom_in` | `Ctrl+=` | sure |  |
| Zoom Out | `timeline.zoom_out` | `Ctrl+-` | sure |  |
| Zoom to Fit | `timeline.zoom_fit` | `Shift+Z` | sure |  |
| Add Video Track | `timeline.add_video_track` | (none) | likely |  |
| Add Audio Track | `timeline.add_audio_track` | (none) | likely |  |

## Track destination and auto track selectors

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Video Destination V1 | `timeline.target_video_1` | `Alt+1` | likely |  |
| Video Destination V2 | `timeline.target_video_2` | `Alt+2` | likely |  |
| Video Destination V3 | `timeline.target_video_3` | `Alt+3` | likely |  |
| Video Destination V4 | `timeline.target_video_4` | `Alt+4` | likely |  |
| Video Destination V5 | `timeline.target_video_5` | `Alt+5` | likely |  |
| Video Destination V6 | `timeline.target_video_6` | `Alt+6` | likely |  |
| Video Destination V7 | `timeline.target_video_7` | `Alt+7` | likely |  |
| Video Destination V8 | `timeline.target_video_8` | `Alt+8` | likely |  |
| Audio Destination A1 | `timeline.target_audio_1` | `Ctrl+Alt+1` | unsure |  |
| Audio Destination A2 | `timeline.target_audio_2` | `Ctrl+Alt+2` | unsure |  |
| Audio Destination A3 | `timeline.target_audio_3` | `Ctrl+Alt+3` | unsure |  |
| Audio Destination A4 | `timeline.target_audio_4` | `Ctrl+Alt+4` | unsure |  |
| Audio Destination A5 | `timeline.target_audio_5` | `Ctrl+Alt+5` | unsure |  |
| Audio Destination A6 | `timeline.target_audio_6` | `Ctrl+Alt+6` | unsure |  |
| Audio Destination A7 | `timeline.target_audio_7` | `Ctrl+Alt+7` | unsure |  |
| Audio Destination A8 | `timeline.target_audio_8` | `Ctrl+Alt+8` | unsure |  |
| Toggle Auto Select V1 | `timeline.auto_select_video_1` | `Ctrl+1` | likely |  |
| Toggle Auto Select V2 | `timeline.auto_select_video_2` | `Ctrl+2` | likely |  |
| Toggle Auto Select V3 | `timeline.auto_select_video_3` | `Ctrl+3` | likely |  |
| Toggle Auto Select V4 | `timeline.auto_select_video_4` | `Ctrl+4` | likely |  |
| Toggle Auto Select V5 | `timeline.auto_select_video_5` | `Ctrl+5` | likely |  |
| Toggle Auto Select V6 | `timeline.auto_select_video_6` | `Ctrl+6` | likely |  |
| Toggle Auto Select V7 | `timeline.auto_select_video_7` | `Ctrl+7` | likely |  |
| Toggle Auto Select V8 | `timeline.auto_select_video_8` | `Ctrl+8` | likely |  |
| Toggle Auto Select A1 | `timeline.auto_select_audio_1` | `Ctrl+Shift+1` | unsure |  |
| Toggle Auto Select A2 | `timeline.auto_select_audio_2` | `Ctrl+Shift+2` | unsure |  |
| Toggle Auto Select A3 | `timeline.auto_select_audio_3` | `Ctrl+Shift+3` | unsure |  |
| Toggle Auto Select A4 | `timeline.auto_select_audio_4` | `Ctrl+Shift+4` | unsure |  |
| Toggle Auto Select A5 | `timeline.auto_select_audio_5` | `Ctrl+Shift+5` | unsure |  |
| Toggle Auto Select A6 | `timeline.auto_select_audio_6` | `Ctrl+Shift+6` | unsure |  |
| Toggle Auto Select A7 | `timeline.auto_select_audio_7` | `Ctrl+Shift+7` | unsure |  |
| Toggle Auto Select A8 | `timeline.auto_select_audio_8` | `Ctrl+Shift+8` | unsure |  |

## Clip

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Enable / Disable Clip | `clip.toggle_enabled` | `D` | sure |  |
| Match Frame (load the source at this frame) | `clip.match_frame` | `F` | sure |  |
| Change Clip Duration | `clip.change_duration` | `Ctrl+D` | sure |  |
| Change Clip Speed | `clip.change_speed` | `R` | likely |  |
| Retime Controls (disabled: no retime curve yet) | `clip.retime_controls` | `Ctrl+R` | sure |  |
| Freeze Frame (disabled: no freeze frames yet) | `clip.freeze_frame` | `Shift+R` | sure |  |
| Link / Unlink Clips | `clip.link` | `Ctrl+Alt+L` | likely | Resolve has one toggle; Kinoklip's Link toggles too. |
| Unlink Clips | `clip.unlink` | (none) | likely |  |
| Add Keyframe (inspector) | `clip.add_keyframe` | `Alt+K` | unsure | kept until the keyframe editor exists |
| Previous Keyframe | `clip.prev_keyframe` | `[` | likely |  |
| Next Keyframe | `clip.next_keyframe` | `]` | likely |  |

## Marking and markers

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Mark In | `mark.in` | `I` | sure |  |
| Mark Out | `mark.out` | `O` | sure |  |
| Mark Clip | `mark.clip` | `X` | sure |  |
| Mark Selection | `mark.selection` | `Shift+A` | sure |  |
| Clear In | `mark.clear_in` | `Alt+I` | sure |  |
| Clear Out | `mark.clear_out` | `Alt+O` | sure |  |
| Clear In and Out | `mark.clear_both` | `Alt+X` | sure |  |
| Go to In | `mark.go_to_in` | `Shift+I` | sure |  |
| Go to Out | `mark.go_to_out` | `Shift+O` | sure |  |
| Add Marker (on the selected clip: clip marker) | `mark.add_marker` | `M` | sure |  |
| Add and Modify Marker | `mark.modify_marker` | `Ctrl+M` | sure |  |
| Delete Marker | `mark.delete_marker` | `Alt+M` | likely |  |

## Pages and view

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Media Page | `view.page_media` | `Shift+2` | sure |  |
| Cut Page (disabled) | `view.page_cut` | `Shift+3` | sure |  |
| Edit Page | `view.page_edit` | `Shift+4` | sure |  |
| Fusion Page (disabled) | `view.page_fusion` | `Shift+5` | sure |  |
| Color Page (disabled) | `view.page_color` | `Shift+6` | sure |  |
| Fairlight Page (disabled) | `view.page_fairlight` | `Shift+7` | sure |  |
| Deliver Page | `view.page_deliver` | `Shift+8` | sure |  |
| Media Pool panel | `view.toggle_media_pool` | (none) | likely |  |
| Effects panel | `view.toggle_effects` | (none) | likely |  |
| Edit Index panel | `view.toggle_edit_index` | (none) | likely |  |
| Mixer panel | `view.toggle_mixer` | (none) | likely |  |
| Metadata panel | `view.toggle_metadata` | (none) | likely |  |
| Inspector panel | `view.toggle_inspector` | (none) | likely |  |
| Single Viewer Mode | `view.single_viewer` | (none) | unsure |  |
| Toggle Source / Timeline Viewer | `view.toggle_source_timeline` | `Q` | sure |  |
| Viewer Full Screen | `view.fullscreen_viewer` | `Ctrl+F` | likely |  |
| Full Screen Window | `view.fullscreen` | `Ctrl+Shift+F` | unsure |  |

## Playback and navigation

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Play / Stop | `transport.play_pause` | `Space` | sure |  |
| Play Reverse | `transport.play_reverse` | `Shift+Space` | likely |  |
| Shuttle Reverse (J, again = faster) | `transport.shuttle_reverse` | `J` | sure |  |
| Stop (K; K+J / K+L step and slow play) | `transport.shuttle_stop` | `K` | sure |  |
| Shuttle Forward (L, again = faster) | `transport.shuttle_forward` | `L` | sure |  |
| Previous Frame | `transport.prev_frame` | `Left` | sure |  |
| Next Frame | `transport.next_frame` | `Right` | sure |  |
| Back One Second | `transport.back_1s` | `Shift+Left` | sure |  |
| Forward One Second | `transport.forward_1s` | `Shift+Right` | sure |  |
| Previous Edit | `transport.prev_edit` | `Up` | sure |  |
| Next Edit | `transport.next_edit` | `Down` | sure |  |
| Go to Timeline Start | `transport.go_to_start` | `Home` | sure |  |
| Go to Timeline End | `transport.go_to_end` | `End` | sure |  |
| Previous Marker | `transport.prev_marker` | `Shift+Up` | sure |  |
| Next Marker | `transport.next_marker` | `Shift+Down` | sure |  |
| Loop | `transport.loop` | `Ctrl+/` | sure |  |
| Play In to Out | `transport.play_in_out` | `Alt+/` | sure |  |
| Play Around Current Frame | `transport.play_around` | `/` | sure |  |

## Help

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| About | `help.about` | (none) | sure |  |
| About Qt | `help.about_qt` | (none) | sure |  |
