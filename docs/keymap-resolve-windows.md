# Keymap: DaVinci Resolve (Windows)

The built-in default keyboard profile (`resources/keymaps/resolve-windows.json`, profile id
`resolve-windows`). It reproduces the default Windows shortcuts of DaVinci Resolve's Edit page and
its global commands, written from general knowledge of how Resolve behaves (nothing copied from
Resolve's documentation). Commands of subsystems Kinoklip does not have yet (Compositing, Color and
Audio pages, audio scrubbing) keep their Resolve key but are
disabled.

**Confidence** says how sure we are that the key is Resolve's default:
- **sure**: well-known default, used daily;
- **likely**: believed correct, not verified against a running Resolve;
- **unsure**: best guess; please correct in the keymap file (and here) if it differs.

Counts: 241 commands; sure 94, likely 83, unsure 64.

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
| New Project | `file.new` | (none) | unsure | no known default (projects are created in Resolve's Project Manager); Ctrl+N is New Timeline |
| New Timeline | `timeline.new` | `Ctrl+N` | likely | File > New Timeline in Resolve; the dialog asks for the name |
| Open Project / Project Manager | `file.open` | `Ctrl+O` / `Shift+1` | likely | Shift+1 is Resolve's Project Manager page; Kinoklip has no project manager yet and opens a project instead. Ctrl+O unsure. |
| Save Project | `file.save` | `Ctrl+S` | sure |  |
| Save Project As | `file.save_as` | `Ctrl+Shift+S` | sure |  |
| Import Media | `file.import_media` | `Ctrl+I` | sure |  |
| Import Subtitle | `file.import_subtitle` | (none) | likely | File > Import > Subtitle in Resolve; no default key known |
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
| Trim Start to Playhead (leaves a gap) | `trim.trim_start` | `Shift+[` | sure |  |
| Trim End to Playhead (leaves a gap) | `trim.trim_end` | `Shift+]` | sure |  |
| Ripple Trim Start to Playhead (closes the gap) | `trim.ripple_trim_start` | `Ctrl+Shift+[` | sure | On a US layout Qt may deliver Ctrl+Shift+{; the timeline accepts both (spec 8.10). |
| Ripple Trim End to Playhead (closes the gap) | `trim.ripple_trim_end` | `Ctrl+Shift+]` | sure | On a US layout Qt may deliver Ctrl+Shift+}; the timeline accepts both (spec 8.10). |
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
| Add Subtitle Track | `timeline.add_subtitle_track` | (none) | likely | Timeline > Add Track > Subtitle Track in Resolve; no default key known |
| Add Subtitle (caption at the playhead) | `timeline.add_subtitle` | (none) | unsure | Resolve adds captions from the subtitle track's right-click menu; no default key known |
| Duplicate Timeline | `timeline.duplicate` | (none) | likely | media pool / Timeline menu command without a default key |
| Delete Timeline | `timeline.delete` | (none) | likely | media pool command without a default key (Delete in the media pool deletes the selected timeline item) |
| Timeline Settings | `timeline.settings` | (none) | unsure | no known default key |
| Next Timeline Tab | `timeline.next_tab` | (none) | unsure | no known Resolve default; click the tab or use the timeline drop-down |
| Previous Timeline Tab | `timeline.prev_tab` | (none) | unsure | no known Resolve default |
| Close Timeline Tab | `timeline.close_tab` | (none) | unsure | no known Resolve default; the tab's close button or its context menu |

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
| Retime Controls (speed bar on the selected clips) | `clip.retime_controls` | `Ctrl+R` | sure |  |
| Freeze Frame (the clip shows the frame at the playhead) | `clip.freeze_frame` | `Shift+R` | sure |  |
| Retime Curve (opens the clip's retime curve in the curve editor) | `clip.retime_curve` | `Ctrl+Shift+R` | likely |  |
| New Compound Clip | `clip.new_compound` | (none) | likely | Clip menu / clip context menu command without a default key in Resolve |
| Open in Timeline (compound clip) | `clip.open_in_timeline` | (none) | likely | context menu command without a default key |
| Decompose in Place (compound clip) | `clip.decompose` | (none) | likely | context menu command without a default key |
| Link / Unlink Clips | `clip.link` | `Ctrl+Alt+L` | likely | Resolve has one toggle; Kinoklip's Link toggles too. |
| Unlink Clips | `clip.unlink` | (none) | likely |  |
| Add Keyframe (inspector) | `clip.add_keyframe` | `Alt+K` | unsure | toggles a key on the last touched inspector parameter; Resolve default not confirmed |
| Add Rectangle Mask | `mask.add_rectangle` | (none) | unsure | Clip > Add Mask; no Resolve default key known, none assigned. In the viewer, editing keys (Delete, Backspace, Enter, Esc) act on the active mask while the viewer has focus. |
| Add Ellipse Mask | `mask.add_ellipse` | (none) | unsure | no default key known |
| Add Polygon Mask (click points in the viewer) | `mask.add_polygon` | (none) | unsure | no default key known |
| Add Bezier Mask (pen: click corners, drag smooth points) | `mask.add_bezier` | (none) | unsure | no default key known |
| Add Freehand Mask (draw in the viewer) | `mask.add_freehand` | (none) | unsure | no default key known |
| Previous Keyframe | `clip.prev_keyframe` | `[` | likely |  |
| Next Keyframe | `clip.next_keyframe` | `]` | likely |  |
| Delete Keyframes | `clip.delete_keyframe` | (none) | unsure | no known default; Delete / Backspace act on selected keys while the keyframe or curve editor has focus |
| Copy Keyframes | `clip.copy_keyframes` | (none) | unsure | no known default; Ctrl+C acts on selected keys while the keyframe or curve editor has focus |
| Paste Keyframes | `clip.paste_keyframes` | (none) | unsure | no known default; Ctrl+V pastes keys at the playhead while the keyframe or curve editor has focus |
| Show Keyframe Editor | `clip.toggle_keyframe_editor` | (none) | likely | Resolve uses the clip's keyframe button; no default key |
| Show Curve Editor | `clip.toggle_curve_editor` | (none) | likely | Resolve uses the clip's curve button; no default key |

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
| Cut Page | `view.page_cut` | `Shift+3` | sure |  |
| Edit Page | `view.page_edit` | `Shift+4` | sure |  |
| Compositing Page (disabled; Resolve: Fusion) | `view.page_compositing` | `Shift+5` | sure |  |
| Color Page (disabled) | `view.page_color` | `Shift+6` | sure |  |
| Audio Page (disabled; Resolve: Fairlight) | `view.page_audio` | `Shift+7` | sure |  |
| Deliver Page | `view.page_deliver` | `Shift+8` | sure |  |
| Media Pool panel | `view.toggle_media_pool` | (none) | likely |  |
| Effects panel | `view.toggle_effects` | (none) | likely |  |
| Edit Index panel | `view.toggle_edit_index` | (none) | likely |  |
| Keyframes panel (keyframe editor window) | `view.toggle_keyframes` | (none) | unsure | Kinoklip's large keyframe editor; no default key is known, none assigned. |
| Stacked Timelines (two timelines one above the other) | `view.stacked_timelines` | (none) | likely | a timeline view option in Resolve; no default key known |
| Mixer panel | `view.toggle_mixer` | (none) | likely |  |
| Metadata panel | `view.toggle_metadata` | (none) | likely |  |
| Inspector panel | `view.toggle_inspector` | (none) | likely |  |
| Single Viewer Mode | `view.single_viewer` | (none) | unsure |  |
| Toggle Source / Timeline Viewer | `view.toggle_source_timeline` | `Q` | sure |  |
| Viewer Full Screen | `view.fullscreen_viewer` | `Ctrl+F` | likely |  |
| On-Screen Mask Controls (viewer on-screen controls: Mask) | `view.onscreen_masks` | (none) | unsure | Resolve picks the viewer overlay from the on-screen controls menu; no default key is known, none assigned. |
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
| Timeline Proxy Resolution: Full | `playback.resolution_full` | (none) | sure | menu command without a default key |
| Timeline Proxy Resolution: Half | `playback.resolution_half` | (none) | sure | menu command without a default key |
| Timeline Proxy Resolution: Quarter | `playback.resolution_quarter` | (none) | sure | menu command without a default key |
| Automatic proxy resolution (Kinoklip) | `playback.resolution_auto` | (none) | sure | no Resolve equivalent; Resolve's closest is Performance Mode |
| Proxy Handling: Prefer Proxies | `playback.proxy_prefer` | (none) | likely | menu command without a default key (Playback > Proxy Handling) |
| Proxy Handling: Prefer Camera Originals | `playback.proxy_originals` | (none) | likely | menu command without a default key |
| Proxy Handling: Disable All Proxies | `playback.proxy_disable` | (none) | likely | menu command without a default key |
| Use Proxy Media If Available | `playback.use_proxy` | (none) | unsure | no default key known; toggles between Prefer Proxies and Disable All Proxies |
| Use Optimized Media If Available | `playback.use_optimized` | (none) | likely | menu command without a default key |
| Render Cache: None | `playback.render_cache_none` | (none) | likely | menu command without a default key |
| Render Cache: Smart | `playback.render_cache_smart` | (none) | likely | menu command without a default key |
| Render Cache: User | `playback.render_cache_user` | (none) | likely | menu command without a default key |
| Delete Render Cache (all) | `playback.delete_render_cache` | (none) | unsure | Resolve offers All / Unused / Selected Clips; Kinoklip deletes all |
| Render Cache Clip Output | `clip.render_cache_output` | (none) | likely | clip context menu command without a default key |
| Clip Attributes (pixel aspect, data levels, field order, rotation, audio channels, start timecode) | `clip.attributes` | (none) | likely | media pool / timeline clip context menu command; no default key known |

## Media pool

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Generate Optimized Media | `mediapool.generate_optimized` | (none) | likely | media pool context menu, no default key |
| Generate Proxy Media | `mediapool.generate_proxy` | (none) | likely | media pool context menu, no default key |
| Stop generating proxy / optimized media | `mediapool.stop_generation` | (none) | unsure | Resolve stops from its progress dialog; Kinoklip from the context menu |
| Link Proxy Media | `mediapool.link_proxy` | (none) | likely | media pool context menu, no default key |
| Unlink Proxy Media | `mediapool.unlink_proxy` | (none) | likely | media pool context menu, no default key |
| Delete Proxy Media | `mediapool.delete_proxy` | (none) | likely | media pool context menu, no default key |
| Delete Optimized Media | `mediapool.delete_optimized` | (none) | likely | media pool context menu, no default key |
| New Bin | `mediapool.new_bin` | `Ctrl+Shift+N` | likely | File > New Bin |
| New Smart Bin | `mediapool.new_smart_bin` | (none) | likely | File menu / bin list context menu, no default key |
| Select Clips in Timeline (media pool selection) | `mediapool.select_in_timeline` | (none) | unsure | media pool context menu; Kinoklip's wording |
| Remove Unused Clips | `mediapool.remove_unused` | (none) | likely | media pool context menu, asks first, no default key |
| Show Bin List (media pool sidebar) | `view.show_bin_list` | (none) | likely | media pool toolbar button, no default key |
| Find in Media Pool (timeline clip) | `timeline.find_in_media_pool` | `Alt+F` | likely | timeline clip context menu |

## Multicam and sync

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Create New Multicam Clip Using Selected Clips (media pool) | `mediapool.new_multicam` | (none) | likely | media pool context menu command without a default key in Resolve |
| Sync and Place on Timeline: Based on Waveform | `mediapool.sync_waveform` | (none) | unsure | Kinoklip's command (Resolve's closest is Auto Sync Audio > Based on Waveform in the media pool); no default key |
| Sync and Place on Timeline: Based on Timecode | `mediapool.sync_timecode` | (none) | unsure | Kinoklip's command (Resolve: Auto Sync Audio > Based on Timecode); no default key |
| Sync and Place on Timeline: Based on In Points | `mediapool.sync_in_points` | (none) | unsure | Kinoklip's command (Resolve: Auto Sync Audio > Based on In Points); no default key |
| Multicam Viewer (source viewer mode: Multicam) | `view.multicam_viewer` | (none) | likely | Resolve picks Multicam in the source viewer's mode menu; no default key is known, none assigned |
| Flatten Multicam Clip | `multicam.flatten` | (none) | likely | timeline context menu command without a default key |
| Multicam Cut Mode | `multicam.mode_cut` | (none) | unsure | Resolve switches cut / switch with buttons above the multicam viewer; no default key is known |
| Multicam Switch Mode | `multicam.mode_switch` | (none) | unsure | see Multicam Cut Mode |
| Multicam: Video and Audio | `multicam.target_both` | (none) | unsure | Resolve's video / audio / both buttons of the multicam viewer; no default key is known |
| Multicam: Video Only | `multicam.target_video` | (none) | unsure | see Multicam: Video and Audio |
| Multicam: Audio Only | `multicam.target_audio` | (none) | unsure | see Multicam: Video and Audio |
| Previous Multicam Angle Page | `multicam.prev_page` | (none) | unsure | no default key is known |
| Next Multicam Angle Page | `multicam.next_page` | (none) | unsure | no default key is known |
| Cut / Switch to Multicam Angle 1 | `multicam.angle_1` | `1` | likely | number keys cut to the angle while the multicam viewer is shown (Resolve's multicam editing); acts on the multicam clip under the timeline playhead |
| Cut / Switch to Multicam Angle 2 | `multicam.angle_2` | `2` | likely |  |
| Cut / Switch to Multicam Angle 3 | `multicam.angle_3` | `3` | likely |  |
| Cut / Switch to Multicam Angle 4 | `multicam.angle_4` | `4` | likely |  |
| Cut / Switch to Multicam Angle 5 | `multicam.angle_5` | `5` | likely |  |
| Cut / Switch to Multicam Angle 6 | `multicam.angle_6` | `6` | likely |  |
| Cut / Switch to Multicam Angle 7 | `multicam.angle_7` | `7` | likely |  |
| Cut / Switch to Multicam Angle 8 | `multicam.angle_8` | `8` | likely |  |
| Cut / Switch to Multicam Angle 9 | `multicam.angle_9` | `9` | likely |  |

## Cut page

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| Smart Insert (insert at the edit point nearest to the playhead) | `cut.smart_insert` | (none) | unsure | Cut page edit button; no own key is known. While the Cut page is shown, Insert Clip (F9) runs Smart Insert (Kinoklip's routing). |
| Append (after the last clip) | `cut.append` | (none) | unsure | Cut page edit button; while the Cut page is shown, Append at End (Shift+F12) runs it. |
| Ripple Overwrite (replace the clip under the playhead) | `cut.ripple_overwrite` | (none) | unsure | Cut page edit button; while the Cut page is shown, Ripple Overwrite (Shift+F10) runs it. |
| Close Up (zoomed cutaway on the track above) | `cut.close_up` | (none) | unsure | Cut page edit button; no default key known, none assigned |
| Place on Top (Cut page) | `cut.place_on_top` | (none) | unsure | Cut page edit button; while the Cut page is shown, Place on Top (F12) runs it. |
| Source Overwrite (on the track above, lined up by timecode) | `cut.source_overwrite` | (none) | unsure | Cut page edit button; no default key known, none assigned |
| Fast Review (play faster, longer clips faster) | `cut.fast_review` | (none) | unsure | Cut page viewer button; no default key known, none assigned |
| Cut Viewer: Source Clip | `cut.viewer_source_clip` | (none) | unsure | Cut page viewer mode button; Q switches between the source and the timeline there |
| Cut Viewer: Source Tape (all clips of the bin back to back) | `cut.viewer_source_tape` | (none) | unsure | Cut page viewer mode button; no default key known |
| Cut Viewer: Timeline | `cut.viewer_timeline` | (none) | unsure | Cut page viewer mode button; Q switches between the source and the timeline there |

## Help

| Command | Action id | Key | Confidence | Note |
|---|---|---|---|---|
| About | `help.about` | (none) | sure |  |
| About Qt | `help.about_qt` | (none) | sure |  |
