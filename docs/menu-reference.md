349 menu entries in 12 menus; 48 panels

# Menu and panel reference

Written by Kinoklip itself (`kinoklip --menu-reference <file>`) from the tables its menus are built from and from the description every panel carries, so it cannot go stale: the test `check_menu_reference` fails when this file differs from what the program writes. Do not edit it by hand; run `scripts/menu-reference.sh` after adding or renaming a menu entry or a panel.

Shortcuts are the defaults of the Resolve keymap (Windows); another keyboard profile or your own changes (Keyboard Customization) do not show here. Entries are listed in menu bar order; a submenu entry is written as `Submenu > Entry`. "(toggle)" entries switch on and off, "(choice)" entries are one of a group. Menus that list files (Open Recent, guide presets) are one row. Commands without a menu entry are in the last menu table. The context menus of the timeline, the media pool and other panels offer the same commands as these menus and are not listed again.

## Kinoklip

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| About Kinoklip |  | Shows the version of Kinoklip, its licence and the libraries it uses. |
| About Qt |  | Shows which version of the Qt toolkit this build uses. |
| Keyboard Customization… | Ctrl+Alt+K | Opens the keyboard settings: pick a keyboard profile, change or clear the key of any command. |
| Preferences… | Ctrl+, | Opens the Preferences window (user settings such as editing, playback, interface and local control). |
| Quit Kinoklip | Ctrl+Q | Closes Kinoklip, asking first whether to save unsaved changes. |

## File

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Project Manager… | Shift+1 | Opens the Project Manager, where projects are created, opened, renamed and deleted. |
| New Project |  | Starts a new, empty project after asking about unsaved changes in the open one. |
| New Timeline… | Ctrl+N | Creates a new timeline in the project, asking for its name and settings. |
| New Bin | Ctrl+Shift+N | Adds a new bin (folder) to the media pool. |
| New Smart Bin… |  | Adds a smart bin: a bin that fills itself with the clips matching rules you set. |
| Open Project… | Ctrl+O | Opens a project file from disk. |
| Open Recent |  | Lists the projects opened lately, newest first; choosing one opens it. |
| Save Project | Ctrl+S | Saves the open project; asks for a file name if it has none yet. |
| Save Project As… | Ctrl+Shift+S | Saves the open project under another name or as another kind (linked or single file). |
| Pack Project… |  | Packs a linked project and all the media it uses into one single-file project. |
| Unpack Project… |  | Writes the media of a single-file project to a folder and continues with a linked project. |
| Compact Project… |  | Rewrites a single-file project without unused media and old versions, to make it smaller. |
| Verify Project File |  | Checks every byte of a single-file project and offers to repair a damaged one. |
| Import > Media… | Ctrl+I | Adds media files to the media pool. |
| Import > Media Folder… |  | Adds a whole folder of media to the media pool, keeping its sub-folders as bins. |
| Import > Timeline… |  | Reads a timeline from an OpenTimelineIO file into the project as a new timeline. |
| Import > Subtitle… |  | Adds a SubRip (.srt) or WebVTT (.vtt) subtitle file to the media pool. |
| Export > Timeline as EDL… |  | Writes the timeline as an edit decision list (EDL) file. |
| Export > Timeline as OpenTimelineIO (.otio)… |  | Writes the timeline as an OpenTimelineIO file for other editors. |
| Export > Timeline Markers to EDL… |  | Writes the timeline markers to an EDL file. |
| Export > Timeline Markers to CSV… |  | Writes the timeline markers to a CSV file that opens in a spreadsheet. |
| Proxy and Optimized Media > Generate Optimized Media |  | Creates optimized media (a file that is easier to play) for the selected clips. |
| Proxy and Optimized Media > Generate Proxy Media |  | Creates proxy media (small, fast copies) for the selected clips. |
| Proxy and Optimized Media > Stop Generating |  | Stops making proxy and optimized media. |
| Proxy and Optimized Media > Link Proxy Media… |  | Links an existing file as the proxy of the selected clip. |
| Proxy and Optimized Media > Unlink Proxy Media |  | Removes the link to the proxy of the selected clips; the file stays. |
| Proxy and Optimized Media > Delete Proxy Media… |  | Deletes the proxy files of the selected clips after asking. |
| Proxy and Optimized Media > Delete Optimized Media… |  | Deletes the optimized media files of the selected clips after asking. |
| Media Pool > Select Clips in Timeline |  | Selects the timeline clips that come from the selected media pool clips. |
| Media Pool > Remove Unused Clips… |  | Removes the media pool clips that no timeline uses, after asking. |
| Project Settings… | Shift+9 | Opens the project settings: timeline resolution, frame rate, colour and other project-wide choices. |

## Edit

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Undo | Ctrl+Z | Takes back the last edit; the entry names the edit. |
| Redo | Ctrl+Shift+Z | Does again the edit that was just taken back. |
| History > Open History Window… |  | Opens a floating list of all undo steps; clicking one goes back or forward to it at once. |
| Cut | Ctrl+X | Removes the selection and keeps it on the clipboard. |
| Copy | Ctrl+C | Copies the selection to the clipboard. |
| Paste | Ctrl+V | Puts the clipboard clips at the playhead, replacing what is there. |
| Paste Insert | Ctrl+Shift+V | Puts the clipboard clips at the playhead, pushing later clips along. |
| Paste Attributes… | Alt+V | Opens a dialog to copy chosen attributes (transform, effects, volume, pan, speed) from the clipboard clip to the selected clips. |
| Ripple Cut | Ctrl+Shift+X | Cuts the selection to the clipboard and closes the gap it leaves. |
| Delete Selected | Backspace | Deletes the selected clips and leaves a gap. |
| Ripple Delete | Del, Shift+Backspace | Deletes the selection and closes the gap it leaves. |
| Delete Gaps |  | Removes the gaps common to the auto-select tracks, keeping them in sync. |
| Select All | Ctrl+A | Selects every clip of the timeline. |
| Deselect All | Ctrl+Shift+A | Clears the selection. |
| Select Clips Forward | Y | Selects every clip from the playhead to the end on the auto-select tracks. |
| Select Clips Backward | Ctrl+Y | Selects every clip from the start to the playhead on the auto-select tracks. |
| Select Clips Forward on All Tracks | Alt+Y | Selects every clip from the playhead to the end on all unlocked tracks. |
| Select Clips Backward on All Tracks | Ctrl+Alt+Y | Selects every clip from the start to the playhead on all unlocked tracks. |
| Insert Clip | F9 | Puts the source clip into the timeline at the playhead, pushing later clips along. |
| Overwrite Clip | F10 | Puts the source clip into the timeline at the playhead, replacing what is there. |
| Ripple Overwrite | Shift+F10 | Replaces the in-to-out range of the timeline with the source and shifts later clips by the difference in length. |
| Place on Top | F12 | Puts the source clip on the first free track above the destination track at the playhead. |
| Replace Clip | F11 | Replaces the clip under the playhead with the source clip, keeping its position and length. |
| Fit to Fill | Shift+F11 | Changes the speed of the source range so it fills exactly the timeline in-to-out range. |
| Append at End | Shift+F12 | Adds the source clip after the last clip of the timeline. |
| Cut Page > Smart Insert |  | Cut page: inserts the source at the edit point nearest the playhead, so no clip is split. |
| Cut Page > Append |  | Cut page: adds the source after the last clip. |
| Cut Page > Ripple Overwrite |  | Cut page: replaces the range around the playhead with the source and shifts later clips. |
| Cut Page > Close Up |  | Cut page: puts the source on top at the playhead, zoomed in on the centre. |
| Cut Page > Place on Top |  | Cut page: puts the source on the track above at the playhead. |
| Cut Page > Source Overwrite |  | Cut page: puts the source on the track above so its timecode matches the clip under the playhead. |
| Cut Page > Fast Review (toggle) |  | Cut page: plays the timeline faster, each shot at a speed that depends on its length. |
| Cut Page > Viewer: Source Clip (choice) |  | Cut page: shows one source clip in the viewer. |
| Cut Page > Viewer: Source Tape (choice) |  | Cut page: shows all source clips as one continuous tape in the viewer. |
| Cut Page > Viewer: Timeline (choice) |  | Cut page: shows the timeline in the viewer. |
| Compositing Page > Add Node… | Shift+Space | Compositing page: opens the node chooser to add a node. |
| Compositing Page > Add Note |  | Compositing page: adds a note to the node graph. |
| Compositing Page > Cut Nodes | Ctrl+X | Compositing page: cuts the selected nodes to the clipboard. |
| Compositing Page > Copy Nodes | Ctrl+C | Compositing page: copies the selected nodes. |
| Compositing Page > Paste Nodes | Ctrl+V | Compositing page: pastes the copied nodes. |
| Compositing Page > Duplicate Nodes | Ctrl+D | Compositing page: duplicates the selected nodes. |
| Compositing Page > Delete Nodes | Del, Backspace | Compositing page: deletes the selected nodes. |
| Compositing Page > Select All Nodes | Ctrl+A | Compositing page: selects all nodes. |
| Compositing Page > Rename Node | F2 | Compositing page: renames the selected node. |
| Compositing Page > Pass Through (toggle) | Ctrl+P | Compositing page: makes the selected node pass its input through unchanged. |
| Compositing Page > Lock Node (toggle) |  | Compositing page: locks the selected node so its settings cannot be changed. |
| Compositing Page > Swap Inputs | Ctrl+T | Compositing page: swaps the two main inputs of the current node. |
| Compositing Page > View in Left Viewer | 1 | Compositing page: shows the selected node in the left viewer. |
| Compositing Page > View in Right Viewer | 2 | Compositing page: shows the selected node in the right viewer. |
| Compositing Page > Fit Nodes to View | Ctrl+F | Compositing page: fits all nodes into the node editor. |
| Compositing Page > Snap Nodes to Grid (toggle) |  | Compositing page: makes dragged nodes snap to a grid. |
| Color Page > Add Serial Node | Alt+S | Color page: adds a serial node after the current node. |
| Color Page > Add Parallel Node | Alt+P | Color page: adds a parallel node next to the current node. |
| Color Page > Add Layer Node | Alt+L | Color page: adds a layer node above the current node. |
| Color Page > Add Serial Node Before | Shift+S | Color page: adds a serial node before the current node. |
| Color Page > Delete Node | Del, Backspace | Color page: deletes the current node. |
| Color Page > Enable / Disable Current Node | Ctrl+D | Color page: switches the grade of the current node off or on. |
| Color Page > Bypass All Grades (toggle) | Shift+D | Color page: switches all grades of the clip off or on, to compare with the original. |
| Color Page > Reset Node Grade | Shift+Home | Color page: resets the grade of the current node. |
| Color Page > Reset Clip Grade | Ctrl+Home | Color page: resets the whole grade of the clip. |
| Color Page > Grade the Timeline (toggle) |  | Color page: grades the whole timeline instead of the clip. |
| Color Page > Add / Remove Keyframe |  | Color page: adds or removes a keyframe at the playhead for the current node. |
| Color Page > Grab Still | Ctrl+Alt+G | Color page: saves the viewer picture with its grade as a still in the gallery. |
| Color Page > Apply Grade from Still |  | Color page: gives the current clip the grade of the selected gallery still. |
| Color Page > Wipe with Still (toggle) | Ctrl+W | Color page: splits the viewer to compare with the selected gallery still. |
| Color Page > Highlight (toggle) | Shift+H | Color page: shows in the viewer what the current node's key selects. |
| Color Page > Copy Grade | Ctrl+C | Color page: copies the grade of the current clip. |
| Color Page > Paste Grade | Ctrl+V | Color page: pastes the copied grade onto the current clip. |

## Trim

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Selection Mode (choice) | A | Selection tool: click and drag clips to select and move them. |
| Trim Edit Mode (choice) | T | Trim tool: drag clip edges to trim, roll or ripple, and slip or slide clips. |
| Blade Edit Mode (choice) | B | Blade tool: click a clip to cut it in two at that point. |
| Dynamic Trim Mode (toggle) | W | Dynamic trim: with an edit point selected, playback moves the edit point along with the playhead. |
| Nudge One Frame Left | , | Moves the selected clips or edit point one frame to the left. |
| Nudge One Frame Right | . | Moves the selected clips or edit point one frame to the right. |
| Nudge Several Frames Left | Shift+, | Moves the selected clips or edit point several frames to the left (the number is set in Preferences). |
| Nudge Several Frames Right | Shift+. | Moves the selected clips or edit point several frames to the right (the number is set in Preferences). |
| Trim Start to Playhead | Shift+[ | Trims the start of the clip under the playhead to the playhead, leaving a gap. |
| Trim End to Playhead | Shift+] | Trims the end of the clip under the playhead to the playhead, leaving a gap. |
| Ripple Trim Start to Playhead | Ctrl+Shift+[ | Trims the start of the clip to the playhead and closes the gap. |
| Ripple Trim End to Playhead | Ctrl+Shift+] | Trims the end of the clip to the playhead and closes the gap. |
| Extend Edit to Playhead | E | Moves the selected edit point to the playhead. |
| Cycle Edit Point Side | U | Cycles the selected edit point between the outgoing side, both sides (roll) and the incoming side. |
| Select Nearest Edit Point | V | Selects the edit point nearest to the playhead; at a cut both sides are selected. |
| Select Nearest Clip or Gap | Shift+V | Selects the clip under the playhead, or the gap if there is no clip. |
| Swap Clip with Previous | Ctrl+Shift+, | Swaps the selected clip with the clip before it. |
| Swap Clip with Next | Ctrl+Shift+. | Swaps the selected clip with the clip after it. |
| Move Clips Up a Track | Alt+Up | Moves the selected clips one track up, replacing what is there. |
| Move Clips Down a Track | Alt+Down | Moves the selected clips one track down, replacing what is there. |

## Timeline

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Split Clip at Playhead | Ctrl+\ | Cuts the clip under the playhead in two (all auto-select tracks when nothing is selected). |
| Join Clips | Alt+\ | Joins the selected edit point or touching clips that continue each other back into one clip. |
| Add Default Transition | Ctrl+T | Adds the default transition at the selected edit point or the clip edge nearest the playhead. |
| Snapping (toggle) | N | Makes clips and the playhead snap to edit points, markers and the playhead while dragging. |
| Linked Selection (toggle) | Ctrl+Shift+L | Makes clips that are linked (such as picture and sound) be selected and moved together. |
| Audio Scrubbing (toggle) | Shift+S | Play a short piece of the sound while the playhead is dragged or stepped |
| Zoom In | Ctrl+= | Zooms the timeline in. |
| Zoom Out | Ctrl+- | Zooms the timeline out. |
| Zoom to Fit | Shift+Z | Zooms the timeline so all of it is visible. |
| Add Video Track |  | Adds a video track to the timeline. |
| Add Audio Track |  | Adds an audio track to the timeline. |
| Add Subtitle Track |  | Adds a subtitle track to the timeline. |
| Add Subtitle |  | Adds a 3-second caption at the playhead on the subtitle track. |
| Delete Empty Tracks |  | Removes every track that has no clips (keeping one video and one audio track and locked tracks). |
| Duplicate Timeline |  | Makes a copy of the current timeline in the project. |
| Delete Timeline |  | Deletes the current timeline from the project. |
| Timeline Settings… |  | Opens the settings of the current timeline. |
| Next Timeline Tab |  | Shows the next open timeline. |
| Previous Timeline Tab |  | Shows the previous open timeline. |
| Close Timeline Tab |  | Closes the tab of the current timeline (the timeline stays in the project). |
| Track Destination and Auto Select > Video Destination V1 | Alt+1 | Makes video track V1 the destination of source edits. |
| Track Destination and Auto Select > Video Destination V2 | Alt+2 | Makes video track V2 the destination of source edits. |
| Track Destination and Auto Select > Video Destination V3 | Alt+3 | Makes video track V3 the destination of source edits. |
| Track Destination and Auto Select > Video Destination V4 | Alt+4 | Makes video track V4 the destination of source edits. |
| Track Destination and Auto Select > Video Destination V5 | Alt+5 | Makes video track V5 the destination of source edits. |
| Track Destination and Auto Select > Video Destination V6 | Alt+6 | Makes video track V6 the destination of source edits. |
| Track Destination and Auto Select > Video Destination V7 | Alt+7 | Makes video track V7 the destination of source edits. |
| Track Destination and Auto Select > Video Destination V8 | Alt+8 | Makes video track V8 the destination of source edits. |
| Track Destination and Auto Select > Audio Destination A1 | Ctrl+Alt+1 | Makes audio track A1 the destination of source edits. |
| Track Destination and Auto Select > Audio Destination A2 | Ctrl+Alt+2 | Makes audio track A2 the destination of source edits. |
| Track Destination and Auto Select > Audio Destination A3 | Ctrl+Alt+3 | Makes audio track A3 the destination of source edits. |
| Track Destination and Auto Select > Audio Destination A4 | Ctrl+Alt+4 | Makes audio track A4 the destination of source edits. |
| Track Destination and Auto Select > Audio Destination A5 | Ctrl+Alt+5 | Makes audio track A5 the destination of source edits. |
| Track Destination and Auto Select > Audio Destination A6 | Ctrl+Alt+6 | Makes audio track A6 the destination of source edits. |
| Track Destination and Auto Select > Audio Destination A7 | Ctrl+Alt+7 | Makes audio track A7 the destination of source edits. |
| Track Destination and Auto Select > Audio Destination A8 | Ctrl+Alt+8 | Makes audio track A8 the destination of source edits. |
| Track Destination and Auto Select > Toggle Auto Select V1 | Ctrl+1 | Switches auto select on or off for video track V1. |
| Track Destination and Auto Select > Toggle Auto Select V2 | Ctrl+2 | Switches auto select on or off for video track V2. |
| Track Destination and Auto Select > Toggle Auto Select V3 | Ctrl+3 | Switches auto select on or off for video track V3. |
| Track Destination and Auto Select > Toggle Auto Select V4 | Ctrl+4 | Switches auto select on or off for video track V4. |
| Track Destination and Auto Select > Toggle Auto Select V5 | Ctrl+5 | Switches auto select on or off for video track V5. |
| Track Destination and Auto Select > Toggle Auto Select V6 | Ctrl+6 | Switches auto select on or off for video track V6. |
| Track Destination and Auto Select > Toggle Auto Select V7 | Ctrl+7 | Switches auto select on or off for video track V7. |
| Track Destination and Auto Select > Toggle Auto Select V8 | Ctrl+8 | Switches auto select on or off for video track V8. |
| Track Destination and Auto Select > Toggle Auto Select A1 | Ctrl+Shift+1 | Switches auto select on or off for audio track A1. |
| Track Destination and Auto Select > Toggle Auto Select A2 | Ctrl+Shift+2 | Switches auto select on or off for audio track A2. |
| Track Destination and Auto Select > Toggle Auto Select A3 | Ctrl+Shift+3 | Switches auto select on or off for audio track A3. |
| Track Destination and Auto Select > Toggle Auto Select A4 | Ctrl+Shift+4 | Switches auto select on or off for audio track A4. |
| Track Destination and Auto Select > Toggle Auto Select A5 | Ctrl+Shift+5 | Switches auto select on or off for audio track A5. |
| Track Destination and Auto Select > Toggle Auto Select A6 | Ctrl+Shift+6 | Switches auto select on or off for audio track A6. |
| Track Destination and Auto Select > Toggle Auto Select A7 | Ctrl+Shift+7 | Switches auto select on or off for audio track A7. |
| Track Destination and Auto Select > Toggle Auto Select A8 | Ctrl+Shift+8 | Switches auto select on or off for audio track A8. |

## Clip

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Enable / Disable Clip | D | Switches the selected clips off or on without deleting them. |
| Flag Clip |  | Adds or removes a coloured flag on the selected clips (the colour is chosen in the timeline tool bar). |
| Match Frame | F | Loads the source of the clip under the playhead into the source viewer at the same frame. |
| Find in Media Pool | Alt+F | Selects the media pool item that the selected clip comes from. |
| Change Clip Duration… | Ctrl+D | Opens a box to set the length of the selected clips. |
| Change Clip Speed… | R | Opens a box to set the speed of the selected clips. |
| Retime Controls | Ctrl+R | Shows the retime controls on the selected clips for changing speed along the clip. |
| Increase Clip Speed by 10% |  | Makes the selected clips 10 percentage points faster. |
| Decrease Clip Speed by 10% |  | Makes the selected clips 10 percentage points slower. |
| Freeze Frame | Shift+R | Makes the selected clips hold the frame under the playhead as a still. |
| Retime Curve | Ctrl+Shift+R | Opens the retime curve of the selected clip, to change its speed over time. |
| New Compound Clip… |  | Combines the selected clips into one compound clip with its own timeline inside. |
| Open in Timeline |  | Opens the timeline inside a compound or nested clip. |
| Decompose in Place |  | Replaces a compound clip by the clips it contains, in the same place. |
| Clip Attributes… |  | Opens the attributes of the selected clip (such as start timecode and audio levels). |
| Open in Compositing Page |  | Opens the selected clip on the Compositing page. |
| Reset Composition |  | Removes the Compositing page node graph from the selected clips. |
| Normalize Audio Levels… |  | Opens a box to change the sound level of the selected clips to a target level. |
| Render Cache Clip Output (toggle) |  | Marks the selected clips to be rendered into the cache when the render cache is set to User. |
| New Multicam Clip Using Selected Clips… |  | Makes a multicam clip from the selected media pool clips, one angle per clip. |
| Sync and Place on Timeline > Based on Waveform |  | Lines up the selected clips by matching their sound and places them on the timeline. |
| Sync and Place on Timeline > Based on Timecode |  | Lines up the selected clips by their timecode and places them on the timeline. |
| Sync and Place on Timeline > Based on In Points |  | Lines up the selected clips by their in points and places them on the timeline. |
| Multicam > Flatten Multicam Clip |  | Replaces the multicam clip by the clips of the angles it currently shows. |
| Multicam > Cut Mode (angle clicks cut at the playhead) (choice) |  | Cut mode: clicking an angle cuts the multicam clip at the playhead and uses that angle after it. |
| Multicam > Switch Mode (angle clicks change the current segment) (choice) |  | Switch mode: clicking an angle changes the angle of the current segment. |
| Multicam > Video and Audio (choice) |  | Angle changes apply to both video and audio. |
| Multicam > Video Only (choice) |  | Angle changes apply to the video only. |
| Multicam > Audio Only (choice) |  | Angle changes apply to the audio only. |
| Multicam > Previous Angle Page |  | Shows the previous page of angles in the multicam viewer. |
| Multicam > Next Angle Page |  | Shows the next page of angles in the multicam viewer. |
| Multicam > Cut / Switch to Angle 1 | 1 | Cuts or switches the multicam clip under the playhead to angle 1 (see Cut Mode and Switch Mode). |
| Multicam > Cut / Switch to Angle 2 | 2 | Cuts or switches the multicam clip under the playhead to angle 2 (see Cut Mode and Switch Mode). |
| Multicam > Cut / Switch to Angle 3 | 3 | Cuts or switches the multicam clip under the playhead to angle 3 (see Cut Mode and Switch Mode). |
| Multicam > Cut / Switch to Angle 4 | 4 | Cuts or switches the multicam clip under the playhead to angle 4 (see Cut Mode and Switch Mode). |
| Multicam > Cut / Switch to Angle 5 | 5 | Cuts or switches the multicam clip under the playhead to angle 5 (see Cut Mode and Switch Mode). |
| Multicam > Cut / Switch to Angle 6 | 6 | Cuts or switches the multicam clip under the playhead to angle 6 (see Cut Mode and Switch Mode). |
| Multicam > Cut / Switch to Angle 7 | 7 | Cuts or switches the multicam clip under the playhead to angle 7 (see Cut Mode and Switch Mode). |
| Multicam > Cut / Switch to Angle 8 | 8 | Cuts or switches the multicam clip under the playhead to angle 8 (see Cut Mode and Switch Mode). |
| Multicam > Cut / Switch to Angle 9 | 9 | Cuts or switches the multicam clip under the playhead to angle 9 (see Cut Mode and Switch Mode). |
| Link Clips | Ctrl+Alt+L | Links the selected clips so they are selected and moved together. |
| Unlink Clips |  | Removes the link between the selected clips. |
| Add Keyframe | Alt+K | Adds a keyframe at the playhead to the parameter being edited on the selected clip. |
| Previous Keyframe | [ | Moves the playhead to the previous keyframe of the selected clip. |
| Next Keyframe | ] | Moves the playhead to the next keyframe of the selected clip. |
| Delete Keyframes |  | Deletes the selected keyframes. |
| Copy Keyframes |  | Copies the selected keyframes. |
| Paste Keyframes |  | Pastes copied keyframes at the playhead. |
| Keyframe Interpolation > Keyframe Linear |  | Gives the selected keyframes linear interpolation. |
| Keyframe Interpolation > Keyframe Ease In |  | Gives the selected keyframes ease-in interpolation. |
| Keyframe Interpolation > Keyframe Ease Out |  | Gives the selected keyframes ease-out interpolation. |
| Keyframe Interpolation > Keyframe Ease In and Out |  | Gives the selected keyframes ease-in and ease-out interpolation. |
| Show Keyframe Editor |  | Shows the keyframe (dope sheet) view of the Keyframes panel. |
| Show Curve Editor |  | Shows the curve view of the Keyframes panel. |
| Add Mask > Rectangle Mask |  | Adds a rectangle mask to the selected clip. |
| Add Mask > Ellipse Mask |  | Adds an ellipse mask to the selected clip. |
| Add Mask > Polygon Mask |  | Adds a polygon mask to the selected clip. |
| Add Mask > Bezier Mask |  | Adds a Bezier mask to the selected clip. |
| Add Mask > Freehand Mask |  | Adds a freehand mask to the selected clip. |

## Mark

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Mark In | I | Sets the in point at the playhead. |
| Mark Out | O | Sets the out point at the playhead. |
| Mark Clip | X | Sets in and out points around the selected clip, or the clip under the playhead. |
| Mark Selection | Shift+A | Sets in and out points around all selected clips. |
| Clear In | Alt+I | Removes the in point. |
| Clear Out | Alt+O | Removes the out point. |
| Clear In and Out | Alt+X | Removes both the in and out points. |
| Go to In | Shift+I | Moves the playhead to the in point. |
| Go to Out | Shift+O | Moves the playhead to the out point. |
| Add Marker | M | Adds a marker at the playhead. |
| Add and Modify Marker… | Ctrl+M | Adds a marker at the playhead, or edits the one there, in a dialog (name, colour, note, length). |
| Delete Marker | Alt+M | Deletes the marker at the playhead. |

## View

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Switch to Page > Media Page (choice) | Shift+2 | Switches to the Media page. |
| Switch to Page > Cut Page (choice) | Shift+3 | Switches to the Cut page. |
| Switch to Page > Edit Page (choice) | Shift+4 | Switches to the Edit page. |
| Switch to Page > Compositing Page (choice) | Shift+5 | Switches to the Compositing page. |
| Switch to Page > Color Page (choice) | Shift+6 | Switches to the Color page. |
| Switch to Page > Audio Page (choice) | Shift+7 | Switches to the Audio page. |
| Switch to Page > Deliver Page (choice) | Shift+8 | Switches to the Deliver page. |
| Show Panel in Workspace > Media Pool (toggle) |  | Shows or hides the Media Pool panel. |
| Show Panel in Workspace > Effects (toggle) |  | Shows or hides the Effects library panel. |
| Show Panel in Workspace > Edit Index (toggle) |  | Shows or hides the Edit Index panel. |
| Show Panel in Workspace > Keyframes (toggle) |  | Shows or hides the Keyframes panel. |
| Show Panel in Workspace > Keyframes Panel: Toggle Keyframe / Curve |  | Switches the Keyframes panel between the keyframe view and the curve view. |
| Show Panel in Workspace > Nodes (toggle) |  | Shows or hides the node editor. |
| Show Panel in Workspace > Gallery (toggle) |  | Shows or hides the Gallery of stills on the Color page. |
| Show Panel in Workspace > Clips (toggle) |  | Shows or hides the clip thumbnail strip on the Color page. |
| Show Panel in Workspace > Timeline (toggle) |  | Shows or hides the mini timeline on the Color page. |
| Show Panel in Workspace > Scopes (toggle) |  | Shows or hides the scopes on the Color page. |
| Show Panel in Workspace > Stacked Timelines (toggle) |  | Shows a second timeline above the first, each with its own tabs, playhead and zoom. |
| Show Panel in Workspace > Mixer (toggle) |  | Shows or hides the Mixer panel. |
| Show Panel in Workspace > Metadata (toggle) |  | Shows or hides the Metadata panel. |
| Show Panel in Workspace > Inspector (toggle) |  | Shows or hides the Inspector panel. |
| Show Panel in Workspace > Show Bin List (toggle) |  | Shows or hides the bin list of the media pool. |
| Show Panel in Workspace > Video Scopes (toggle) | Ctrl+Shift+W | Opens a floating window with scopes of the picture on the viewer. |
| Viewer > Single Viewer Mode (toggle) |  | Shows one viewer instead of the source and timeline viewers side by side. |
| Viewer > Toggle Source / Timeline Viewer | Q | Switches the single viewer between the source clip and the timeline. |
| Viewer > Multicam Viewer (source viewer shows the angles) (toggle) |  | Makes the source viewer show all angles of the multicam clip. |
| Viewer > Full-Screen Viewer | Ctrl+F | Shows the viewer picture full screen. |
| Viewer > Viewer Zoom to Fit | Z | Fits the picture to the viewer. |
| Viewer > On-Screen Transform Controls (toggle) |  | Shows controls in the viewer to move, scale and rotate the selected clip. |
| Viewer > On-Screen Mask Controls (toggle) |  | Shows controls in the viewer to edit the masks of the selected clip. |
| Viewer > On-Screen Crop Controls (toggle) |  | Shows controls in the viewer to crop the selected clip. |
| Viewer > On-Screen Dynamic Zoom Controls (toggle) |  | Shows controls in the viewer to set the start and end framing of dynamic zoom. |
| Viewer Overlays > Show Rulers (toggle) |  | Shows rulers along the top and left edges of the viewer picture; new guides are dragged out of them. |
| Viewer Overlays > Guides > Show Guides (toggle) |  | Shows or hides the guides over the viewer picture. |
| Viewer Overlays > Guides > Lock Guides (toggle) |  | Locks the guides so they cannot be moved by accident. |
| Viewer Overlays > Guides > Snap to Guides (toggle) |  | Makes an on-screen transform drag snap the picture to the guides. |
| Viewer Overlays > Guides > Add Horizontal Guide |  | Adds a horizontal guide in the middle of the picture. |
| Viewer Overlays > Guides > Add Vertical Guide |  | Adds a vertical guide in the middle of the picture. |
| Viewer Overlays > Guides > Clear Guides |  | Removes all guides. |
| Viewer Overlays > Guides > Guide Color… |  | Opens a colour chooser for the colour of the guides. |
| Viewer Overlays > Guides > Load Guide Preset |  | Lists the built-in and saved guide presets; choosing one applies it to the viewer. |
| Viewer Overlays > Guides > Save Guides as Preset… |  | Saves the current guides as a named preset. |
| Viewer Overlays > Guides > Delete Guide Preset |  | Lists the guide presets you saved; choosing one deletes it. |
| Viewer Overlays > Safe Area > Action Safe (90 %) (toggle) |  | Shows the action-safe area (90 percent of the picture) over the viewer. |
| Viewer Overlays > Safe Area > Title Safe (80 %) (toggle) |  | Shows the title-safe area (80 percent of the picture) over the viewer. |
| Viewer Overlays > Safe Area > Centre Mark (toggle) |  | Shows a mark at the centre of the picture. |
| Viewer Overlays > Safe Area > No Aspect Marker (choice) |  | Shows no aspect-ratio marker over the viewer. |
| Viewer Overlays > Safe Area > 2.39:1 (Widescreen) (choice) |  | Shows a 2.39:1 widescreen frame over the viewer. |
| Viewer Overlays > Safe Area > 1.85:1 (Flat) (choice) |  | Shows a 1.85:1 flat frame over the viewer. |
| Viewer Overlays > Safe Area > 4:3 (choice) |  | Shows a 4:3 frame over the viewer. |
| Viewer Overlays > Safe Area > 1:1 (Square) (choice) |  | Shows a 1:1 square frame over the viewer. |
| Viewer Overlays > Safe Area > 4:5 (Portrait) (choice) |  | Shows a 4:5 portrait frame over the viewer. |
| Viewer Overlays > Safe Area > 9:16 (Vertical Video) (choice) |  | Shows a 9:16 vertical-video frame over the viewer. |
| Viewer Overlays > Show Grid (toggle) |  | Shows or hides the grid over the viewer picture. |
| Viewer Overlays > Grid Type > Rule of Thirds (choice) |  | Uses a rule-of-thirds grid. |
| Viewer Overlays > Grid Type > 4 x 4 Grid (choice) |  | Uses a 4 x 4 grid. |
| Viewer Overlays > Grid Type > Custom Grid… (choice) |  | Opens a box to set the number of grid columns and rows (2 to 32). |

## Playback

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Play / Pause | Space | Starts or stops playback. |
| Play Reverse | Shift+Space | Plays backwards. |
| Shuttle Reverse | J | Plays backwards; each further press doubles the speed. |
| Stop | K | Stops playback. |
| Shuttle Forward | L | Plays forwards; each further press doubles the speed. |
| Record Voice-Over | Ctrl+Alt+R | Counts in, then plays the timeline from the playhead and records the microphone onto the armed audio track. Press Space or Stop to end the take. |
| Previous Frame | Left | Moves the playhead back one frame. |
| Next Frame | Right | Moves the playhead forward one frame. |
| Back One Second | Shift+Left | Moves the playhead back one second. |
| Forward One Second | Shift+Right | Moves the playhead forward one second. |
| Previous Edit Point | Up | Moves the playhead to the previous edit point. |
| Next Edit Point | Down | Moves the playhead to the next edit point. |
| Go to Start | Home | Moves the playhead to the start of the timeline. |
| Go to End | End | Moves the playhead to the end of the timeline. |
| Previous Marker | Shift+Up | Moves the playhead to the previous marker. |
| Next Marker | Shift+Down | Moves the playhead to the next marker. |
| Loop Playback (toggle) | Ctrl+/ | Repeats playback instead of stopping at the end of the timeline. |
| Play In to Out | Alt+/ | Plays from the in point to the out point. |
| Play Around Current Frame | / | Plays a short stretch before and after the playhead and returns to it. |
| Playback Resolution > Full Resolution (choice) |  | Play every frame at full resolution; frames are skipped if the computer cannot keep up. |
| Playback Resolution > Half Resolution (choice) |  | Play the viewer at half the timeline resolution, for smoother playback. |
| Playback Resolution > Quarter Resolution (choice) |  | Play the viewer at a quarter of the timeline resolution, for smoother playback. |
| Playback Resolution > Automatic (Half When Playback Cannot Keep Up) (choice) |  | Play at full resolution and drop to half only while playback cannot keep up; paused frames stay full. |
| Proxy Handling > Prefer Proxies (choice) |  | Plays proxy media where it exists. |
| Proxy Handling > Prefer Camera Originals (choice) |  | Plays the camera original files even where proxy media exists. |
| Proxy Handling > Disable All Proxies (choice) |  | Ignores all proxy media. |
| Use Proxy Media If Available (toggle) |  | Uses proxy media for playback when a proxy exists. |
| Use Optimized Media If Available (toggle) |  | Uses optimized media for playback when it exists. |
| Render Cache > None (choice) |  | Does not cache rendered sections. |
| Render Cache > Smart (choice) |  | Caches sections that need heavy work (transitions, composites, effects, speed changes) automatically. |
| Render Cache > User (choice) |  | Caches only the clips marked with Render Cache Clip Output. |
| Delete Render Cache… |  | Deletes the cached renders after asking. |

## Audio

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Add Bus |  | Adds a stereo audio bus to the mixer. |
| Reset Loudness Meter |  | Restarts the loudness measurement. |
| Float Viewer (toggle) |  | Puts the small viewer of the Audio page in its own window, and back. |

## Workspace

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Full Screen Window (toggle) | Ctrl+Shift+F | Makes the window fill the whole screen. |
| Reset UI Layout |  | Puts the panels of every page back to their default places and sizes. |
| Color Theme |  | Lists the colour schemes found on this computer; choosing one changes the colours of the whole window. |
| Style |  | Lists the widget styles found on this computer; choosing one changes how buttons and panels are drawn. |

## Help

| Entry | Shortcut (default Resolve keymap) | What it does |
|---|---|---|
| Help for This… | F1 | Opens the guide page about the panel or page under the pointer. |
| Guide… |  | Opens the Kinoklip guide. |
| Getting Started |  | Opens the getting-started page of the guide. |
| Report a Bug or Inconsistency… |  | Shows what a bug report would contain and opens the report form on GitHub. |
| Join the Community on Discord |  | Opens the Kinoklip community on Discord in the web browser. |
| Export Log File… |  | Saves what Kinoklip did in this session (system facts, decoder for each file, playback statistics, warnings) as a text file to attach to a bug report. |

## Panels

### Media page

| Panel | What it is |
|---|---|
| Media Pool | The project's media in bins (Master, smart bins, power bins), as posters or a list, with search and colour filter; clips are dragged from here to the timeline. |
| Media Storage | Browses the folders of this computer and adds media files to the media pool. |
| Metadata | Shows the details of the selected media and lets you edit its shot, scene and notes fields. |
| Source Viewer | Shows the clip chosen in the media pool, with in and out points and a scrub bar. |

### Cut page

| Panel | What it is |
|---|---|
| Cut Tool Bar | The smart edit buttons, split, ripple delete, snapping, quick dissolve, Fast Review and zoom. |
| Cut Viewer | The single large viewer, showing one source clip, all source clips as one tape, or the timeline. |
| Detail Timeline | The timeline zoomed in around a centred playhead, where clips are edited. |
| Media Pool | The project's media in bins (Master, smart bins, power bins), as posters or a list, with search and colour filter; clips are dragged from here to the timeline. |
| Overview Timeline | The whole timeline fitted to the width; click or drag to move the playhead. |
| Source Viewer | Shows the clip chosen in the media pool, with in and out points and a scrub bar. |
| Timeline Viewer | Shows the picture of the timeline at the playhead, with transport controls. |

### Edit page

| Panel | What it is |
|---|---|
| Edit Index | Table of the timeline's events or markers; clicking a row moves the playhead there. |
| Effects | Searchable library of video transitions, titles, generators and video and audio effects; drag an entry onto the timeline or double-click it to apply it. |
| Inspector | Edits the parameters of the selected clip on the video, audio, effects, title and transition tabs. |
| Keyframes | Editor for the keyframes of the selected clips: a parameter list beside a dope sheet or a curve view on the timeline's time. |
| Media Pool | The project's media in bins (Master, smart bins, power bins), as posters or a list, with search and colour filter; clips are dragged from here to the timeline. |
| Metadata | Shows the details of the selected media and lets you edit its shot, scene and notes fields. |
| Mixer | A strip for each audio track and each bus, with its level and meter. |
| Source Viewer | Shows the clip chosen in the media pool, with in and out points and a scrub bar. |
| Timeline | The timeline with its tool bar: tracks, clips, editing tools, snapping and zoom. |
| Timeline Viewer | Shows the picture of the timeline at the playhead, with transport controls. |

### Compositing page

| Panel | What it is |
|---|---|
| Inspector | Parameters of the selected node, with an on/off switch and Reset. |
| Left Viewer | Shows the output of one node of the open composition. |
| Media Pool | The project's media in bins (Master, smart bins, power bins), as posters or a list, with search and colour filter; clips are dragged from here to the timeline. |
| Node Editor | The node graph of the open clip: select, move, connect and edit nodes. |
| Nodes | Searchable list of every node type; drag one onto the node editor or double-click it to add it. |
| Right Viewer | Shows the output of one node of the open composition. |
| Transport | Play and stop, and a ruler over the frames of the open clip. |

### Color page

| Panel | What it is |
|---|---|
| Clips | Every clip of the timeline as a thumbnail; click one to grade it. |
| Curves | Curve adjustments of the current node. |
| Gallery | The stills grabbed from graded clips; use one to compare in the viewer or copy its grade. |
| Key | The key output gain of the current node, qualifier invert and a small view of the key. |
| LUTs | Browser of .cube look-up tables; apply one to the current node. |
| Nodes | The node graph of the grade of the current clip. |
| Primaries | Primary colour correction of the current node. |
| Qualifier | Selects part of the picture by hue, saturation and luminance for the current node. |
| Scopes | A scope of the picture; choose the type from its menu. |
| Timeline | The video tracks fitted to the width; click or drag to move the playhead. |
| Viewer | The graded picture at the playhead, with transport controls. |
| Windows | Circle, linear, polygon and gradient windows that limit the current node to part of the picture. |

### Audio page

| Panel | What it is |
|---|---|
| Audio Timeline | The timeline with tall audio tracks, level lines and fade handles. |
| Meters | Output and bus meters, and the loudness of the output bus (momentary, short-term, integrated, true peak). |
| Mixer | A strip for each audio track and each bus, with its level and meter. |
| Timeline Viewer | Shows the picture of the timeline at the playhead, with transport controls. |

### Deliver page

| Panel | What it is |
|---|---|
| Render Queue | One card for each render job with its progress; Render All, Stop and Clear Finished. |
| Render Range | The timeline overview under the viewer, showing the range that will be rendered. |
| Render Settings | Preset, file name and location, video, audio and file options and render range of the export; Add to Render Queue is here. |
| Timeline Viewer | Shows the picture of the timeline at the playhead, with transport controls. |
