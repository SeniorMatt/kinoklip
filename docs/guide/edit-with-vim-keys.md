# Edit with vim keys

If you live in Vim or Neovim, Kinoklip has a **Vim mode** that edits the timeline with the keys you
already know: `5l` steps five frames, `w` jumps to the next clip, `dd` removes the clip under the
playhead, `u` undoes. It is an option, off by default, and it works with every keyboard profile
(Resolve, Premiere Pro, ...): it does not replace the profile.

## Switch it on

Either tick **Vim mode** in **Preferences > Keyboard**, or choose **Kinoklip > Vim Mode**. The mode
line appears at the left of the page bar, on every page:

- **NORMAL**: keys are Vim commands. Nothing is typed.
- **VISUAL**, **V-LINE**, **V-BLOCK**: `v`, `V` and `Ctrl+V` start selecting; motions extend the selection.
- **INSERT**: **`i`** (or `a`) switches to it. Your keyboard profile's own shortcuts work again
  (Space plays, `J` `K` `L` shuttle, `I` and `O` mark, ...). **Esc** (or **Ctrl+[**) comes back.
  Any text field is INSERT automatically: typing `dd` in a name field types two letters.
- **COMMAND**: `:` opens the command line in the same row; Enter runs it, Esc closes it.

The mode line also shows the keys you have typed so far (`3d`) and the panel that has the keyboard
focus; that panel gets an accent frame. The keys work in the **timeline and the viewers**, in every
**list and tree** (Media Pool, Effects, Edit Index, ...) and, for the keys that move between panels,
pages and the command line, in every other panel. Text fields and dialogs keep their own keys.

## Esc

Esc always undoes the innermost thing first: half-typed keys and a count, then the command line, then
VISUAL, then INSERT. With nothing to undo it is not taken at all (it cancels a drag or closes a
popup, as always); if the focus is on a field or a control inside a panel it goes back to the panel.
In a text field Esc keeps what you typed and leaves the field; in a numeric field it cancels the
typing, as it does everywhere in Kinoklip. **Ctrl+C** acts as Esc only while something is pending,
in VISUAL or in COMMAND; otherwise it stays Copy.

## The timeline is a text

Vim edits text; Kinoklip's Vim mode edits the timeline as if it were text:

- **Time is the line.** A column is a frame. `l` is one frame to the right.
- **The tracks are the lines**, the top track first: the video tracks from the highest to V1, then
  A1, A2, ... `j` and `k` move the **cursor track** down and up (audio tracks too). The cursor track is
  tinted, and it is where the source edits (`Space` `s` `i` ...) go.
- **Clips are words**, gaps are white space, markers are headings.
- The **cursor** is the playhead on the cursor track.

## The keys

Type a count first to repeat: `5l`, `3w`, `2dd`, `3u`. **Esc** drops what you have typed.

**Moving.** Every motion takes a count, and also works after an operator and in VISUAL.

| Keys | Goes to |
|---|---|
| `h` `l` | previous / next frame |
| `{` `}` | one second back / forward |
| `0` `$` | start / end of the timeline |
| `{n}\|` | frame n (`120\|`) |
| `w` `b` | start of the next clip / of this clip, or the previous one, on the cursor track |
| `e` `ge` | last frame of this clip (or the next) / of the previous clip |
| `W` `B` `E` `gE` | the same across **all** tracks: next / previous edit point, last frame before one |
| `^` | start of the first clip |
| `]]` `[[` | next / previous marker |
| `]k` `[k` | next / previous keyframe of the clip under the cursor |
| `j` `k` | cursor track down / up |
| `gg` `G` `{n}G` | first (top) / last (bottom) / n-th track |
| `m` + letter, `'` + letter, `''` | remember the cursor (frame and track), jump to it, jump back |
| `H` `L` | previous / next timeline tab |
| `zi` `zo` `zf` | zoom in, out, to fit |
| `n` `N` | next / previous search match (`/` searches marker and clip names) |

**Changing.** Operators take a motion or a text object: `dw`, `d$`, `y]]`, `c2e`, `diw`.

| Keys | What it does |
|---|---|
| `d` | ripple delete: later material closes up |
| `c` | delete leaving a gap, then INSERT (`cw` changes the clip, like `ce`) |
| `y` | copy |
| `dd` `yy` `cc` | the clip under the cursor (`3dd`: and the next two); in a gap, the gap |
| `x` `X` | ripple delete / delete leaving a gap: the selection, else the clip under the cursor |
| `D` | ripple delete from the cursor to the end of the clip |
| `p` `P` | paste at the cursor, inserting / overwriting (`3p`: three times) |
| `s` `J` `~` | split at the cursor, join with the next clip, enable / disable the clip |
| `r` `R` | replace / ripple overwrite the clip with the source clip |
| `>` `<` | nudge the clip a frame right / left (count: frames) |
| `.` | repeat the last change (`3.` with a new count) |
| `u` `Ctrl+R` | undo / redo (`3u`: three steps) |
| `M` | add a marker |
| `i` `a` | INSERT mode |
| `v` `V` `Ctrl+V` | VISUAL: frames, clips, a block of tracks |
| `:` | command line |
| `Space` `Space` | play / pause |
| `Space` `s` `i` / `o` / `a` | insert / overwrite / append the clip from the Source viewer |
| `Space` `y` | copy the timecode of the cursor to the system clipboard |

**Text objects**, after an operator (or in VISUAL):

| Keys | Selects |
|---|---|
| `iw` | the clip under the cursor on the cursor track; in a gap, the gap. `d2iw`: two clips |
| `aw` | the clip and the gap after it |
| `iW` `aW` | the clip and the clips linked to it, on every track |
| `ip` `ap` | the run of touching clips (and the gap after it) |
| `im` | the span between the markers around the cursor, on every track |
| `ir` | the In to Out range, on every track |
| `it` | the clips at the cursor frame on every track (the stack) |

**Registers.** `"a` before an operator or `p` names a register: `"ayy` copies the clip into `a`, `"ap`
pastes it. `"A` is `"a`. Without a register the editor's clipboard is used (so Ctrl+V pastes it too),
`"0` holds the last copy and `"_` throws the text away.

**Counts multiply**: `2d3w` is `d6w`. `dd` with a count takes that many clips.

Every Vim command is **one undo step**: `3dd`, `d2w`, `3p`, `.` and `vjd` each take back with one `u`.
Operators work on the **cursor track**; with *Linked Selection* on (`Space` `o` `l`) the tracks of
linked clips (picture and sound) are cut together. A **locked** track is never changed. Where the
text says every track (`W`, `im`, `ir`), the tracks with *Auto Select* on are cut.

## Panels, windows and pages

The panels are the windows of the editor. Move the keyboard focus the way you move between Neovim
windows; the panel with the focus has the accent frame and its name is in the mode line.

| Keys | What it does |
|---|---|
| `Ctrl+H` `Ctrl+J` `Ctrl+K` `Ctrl+L` | focus the panel to the left / below / above / right (the nearest one in that direction, by where it is on the screen) |
| `Ctrl+W` + `h` `j` `k` `l` | the same |
| `Ctrl+W` `w` / `W` | the next / previous panel in reading order (top to bottom, then left to right), wrapping round; `3` `Ctrl+W` `w` goes to the third |
| `Ctrl+W` `p` | the panel that had the focus before |
| `Ctrl+W` `o` | zoom: hide the other panels; again brings them back (`:only` does it too) |
| `Ctrl+W` `=` | bring the hidden panels back and reset the layout |
| `Ctrl+W` `c` | hide this panel (the same as its button in the interface bar) |
| `Space` `w` + `m` `e` `i` `t` `v` `k` `x` `n` `d` `l` | focus the Media Pool, Effects, Inspector, timeline, viewer, Keyframes, Mixer, Nodes, Metadata, Edit Index; a hidden panel is shown first |
| `gt` / `gT` | the next / previous page, wrapping round |
| `3gt` | page 3 (1 Media, 2 Cut, 3 Edit, 4 Compositing, 5 Color, 6 Audio, 7 Deliver); `2gT` goes two pages back |

`:page edit`, `:page next` and `:panel inspector` do the same by name. After a page change the focus
lands on the same kind of panel (else the first one). A panel made of fields and buttons (the
Inspector) takes only the global keys for now: `j` does nothing there, the fields work as usual.

## Lists and trees

Every list, tree and grid (the Media Pool's bins and clips, the Effects library, the Edit Index, the
render queue, ...) works with the same keys; no panel needed code for it. A row is a line: `j` and `k`
move over the rows of the view, a tree is read as drawn (closed branches skipped).

| Keys | What it does |
|---|---|
| `j` `k` `gg` `G` `{n}G` | next / previous row, first row, last row, row n (`dj` takes this row and the next, `d3j` four) |
| `Ctrl+D` `Ctrl+U` | half a page down / up |
| `o` / `l` | open: a bin, a clip in the Source viewer, an effect applied (`l` first opens a closed branch) |
| `h` / `-` | close an open branch, else go to the parent row; in the Media Pool, up to the parent bin |
| `za` `zo` `zc` `zR` `zM` | toggle / open / close this branch, open / close all branches |
| `zt` `zz` `zb` | scroll the row to the top / middle / bottom |
| `V` (or `v`) | select rows (`j` `k` grow it, `o` goes to the other end, Esc leaves) |
| `dd` `x` `d{motion}` | delete the rows: it presses the panel's own Delete key, so a panel that asks first (media used in the timeline) still asks |
| `yy` `y{motion}` | copy the names of the rows to the system clipboard (`2yy` two rows) |
| `p` | the panel's own paste: the Media Pool imports the files on the clipboard |
| `a` / `r` | new bin / rename (F2), Neo-tree's keys |
| `/` `n` `N` | the panel's search field gets the focus (type to filter; `Ctrl+J` `Ctrl+K` move over the rows, Enter gives the list the focus, Esc too); `n` and `N` then step to the next and previous row. In a list without a search field `/` asks for text and `n` `N` find the next and previous row with that text |
| `.` | repeat the last change |

Counts, VISUAL and the registers are Vim's: `3dd`, `Vjjd`. The arrow keys, Enter and F2 keep working.

**Dialogs** keep the Qt keys: Tab, arrows, Enter and Esc work, and Vim keys are not taken. In a list
inside a dialog `Ctrl+J` and `Ctrl+K` also move down and up (from the filter field above the list too).

## The Inspector and other panels of fields

The Inspector, the Compositing Inspector, Render Settings, Metadata and the Color page's panels are
**forms**: a column of rows (a label, a value, a keyframe diamond, a reset button). Press `Space w i` (or `Ctrl+L` from the viewer) to focus the Inspector. A frame marks the row you are on; the keyboard focus stays on the panel, so `j` and
`k` never type into a field.

| Keys | What it does |
|---|---|
| `j` `k` `gg` `G` `{n}G` | next / previous row, first / last row, row n |
| `Ctrl+A` `Ctrl+X` | raise / lower the value by one step; `5 Ctrl+A` five steps. A choice goes to the next / previous entry, a check box toggles |
| `g Ctrl+A` `g Ctrl+X` / `zl` `zh` | ten steps / a tenth of a step |
| `h` `l` | on a row with a single value, one step down / up (on a line with several buttons, move between them) |
| `i` `a` `cw` `Enter` | type the value: a number gets its line edit, a text (the title text too) its box. You are in INSERT while you type; Enter keeps it, Esc leaves it and gives the panel back its keys |
| `Enter` `x` | toggle a check box, open a choice, a colour or a file dialog, press a button, fold a section; `x` on a section header switches the effect on or off |
| `dd` | reset the value to its default; on a section header remove the effect, mask or transition |
| `yy` `p` | copy the value, paste it into this row (select another clip first to copy across clips) |
| `.` | repeat the last change |
| `Space k k` | add a keyframe on this parameter at the playhead, or remove the one there |
| `]k` `[k` | playhead to the next / previous keyframe of this parameter |
| `Space k l` `i` `o` `b` `s` `h` | the keyframe at the playhead: linear, ease in, ease out, ease in and out, smooth, hold |
| `Space k d` | delete the keyframe at the playhead |
| `za` `zo` `zc` `zR` `zM` | fold / open / close the section, open / close all |
| `]]` `[[` | next / previous tab (Video, Audio, Effects, Title, Transition) |
| `>>` `<<` | move the effect or mask down / up in its stack |
| `K` | what the parameter does |

Controls that are not parameters (the buttons, boxes and fields of Render Settings, for one) are rows
too; the same keys work on them.

## Space is the leader

Space starts a second group of keys. Wait half a second after it (or after `g`, `z`) and a list
appears above the page bar with every key that can follow and what it does (the "which-key" list):

| Keys | What it does |
|---|---|
| `Space` `Space` | play / pause |
| `Space` `s` + `i` `o` `a` `r` `f` `t` | source edits: insert, overwrite, append, replace, fit to fill, ripple overwrite |
| `Space` `m` + `a` `m` `d` `c` | markers: add, modify, delete, mark the clip |
| `Space` `i` + `i` `o` `p` | Mark In, Mark Out, play In to Out |
| `Space` `k` + `k` `d` `l` `i` `o` `b` | keyframes: add, delete, linear, ease in, ease out, both |
| `Space` `e` `e` | add the default transition |
| `Space` `t` + `v` `a` `d` | add a video track, an audio track, delete empty tracks |
| `Space` `o` + `s` `l` `a` `L` | snapping, linked selection, audio scrubbing, loop |
| `Space` `f` + `f` `c` `e` `t` `m` `d` `h` `k` `p` | pickers: media, commands, effects, timelines, markers, bins, help, Vim keys, projects (see Pickers) |
| `Space` `w` + `m` `e` `i` `t` `v` `k` `x` `n` `d` `l` | focus a panel (see Panels, windows and pages) |
| `Space` `n` / `x` | show the Media Pool / close the timeline tab |
| `g` `j` `k` `l` | shuttle reverse, stop, forward |

The full list is the [cheat sheet](../keymaps/vim-keys.md).

## Change the keys

The keys are a table, and you can change every one.

- **Kinoklip > Keyboard Customization > Vim keys** (or **Vim keys...** in Preferences > Keyboard) shows
  them by place (Leader, Global, Timeline, Viewer). **Add...**, **Change...** and **Remove** edit a row,
  **Reset** brings one back, **Reset All** all of them. Keys are written as in Vim (`gg`,
  `<leader>ff`, `<C-h>`, `<Space>`, `<CR>`); the command is a command id from the Shortcuts tab
  (`edit.undo`), `vim:name`, or `:` and a command line. Saving checks the keys: one that is already
  bound is refused unless you replace it, and a key that is also the start of longer ones is
  allowed with a warning (it waits for `timeoutlen`, 400 ms, for the next key; `:set timeoutlen=300`).
  **Export...** and **Import...** move your changes to another computer as a file.
- On the command line: `:nmap Q edit.undo`, `:nmap <leader>w :w<CR>`, `:nunmap p`, `:map` lists
  yours, `:set leader=,`.
- In a text file named `vimrc` in Kinoklip's configuration folder (the **Open vimrc** button creates
  it): one command per line, `"` starts a comment. It is plain text, so a dotfiles manager can keep
  it. Its keys show in the tab, read only. `:source` reads it again.

## A Russian (or other) layout

Keys match their place on the keyboard when the layout is not Latin, so `д` is `l`, `ц` is `w` and
`Shift+х` is `{` without switching to English. A German or French layout is not changed: `z` is the
key that types `z`. `:set nophysicalkeys` turns this off.

## Visual mode

`v` starts selecting **frames** on the cursor track, `V` selects **whole clips**, `Ctrl+V` a **block**
of frames across several tracks (`j` and `k` add tracks). The selection is painted on the timeline,
the mode line says `VISUAL`, `V-LINE` or `V-BLOCK`. Move with any motion; `o` goes to the other end,
`iw`, `ip`, `im` and the other text objects select that object. Then `d` or `x` ripple deletes,
`X` deletes leaving a gap, `c` the same and goes to INSERT, `y` copies and `p` replaces the selection
with a register. `.` repeats the change on the same amount. **Esc** (or the same key) leaves, `gv`
selects the last selection again.

## The command line

`:` opens a line in the page bar row, in place of the mode line. Enter runs it, Esc closes it (so do
Ctrl+C, Ctrl+[ and Backspace on an empty line). The result shows in the mode line; a failure is red.

**Completion.** **Tab** completes the word before the cursor: command names, action ids
(`:action edit.un` -> `edit.undo`), pages, timeline names, options, and the timecodes of the markers
after a number. One match is filled in; several are listed above the line and extended to what they
share, then **Tab** and **Shift+Tab** (or Ctrl+N and Ctrl+P) step through them.

**History.** **Up** and **Down** bring back the older and newer lines that start with what you have
typed; Shift+Up, Page Up (or Ctrl+P, Ctrl+K) and Shift+Down, Page Down (Ctrl+N, Ctrl+J) take any line.
The history survives a restart (200 lines; `/` keeps its own). **`q:`** in NORMAL opens it as a
picker, newest first; choosing a line runs it. Editing: Ctrl+B and Ctrl+E go to the start and the end,
Ctrl+H and Ctrl+L one character left and right, Ctrl+W deletes the word before the cursor, Ctrl+U
everything before it.

| Command | What it does |
|---|---|
| `:w` `:wa` | save the project |
| `:q` `:qa` | quit (asking about unsaved changes as usual); `:q!` quits without asking |
| `:wq` `:x` `:wqa` `:xa` | save and quit |
| `:e {name}` | open the timeline with that name in a tab; `:e {file}` opens a project file; `:e` alone opens the Project Manager |
| `:enew {name}` `:new {name}` | a new timeline (the name is optional) |
| `:bn` `:bp` `:bd` | next / previous / close the timeline tab |
| `:b {n or name}` `:ls` | switch to tab n or a timeline by (a part of) its name; list the tabs |
| `:120` `:+10` `:-5` `:$` `:00:01:10:00` | go to a frame, forward / back some frames, the end, or a timecode |
| `:page {name}` | `media cut edit comp color audio deliver`, `next`, `prev` or a number |
| `:undo [n]` `:redo [n]` `:history` | undo or redo n steps; the edit history window |
| `:render` `:deliver` | go to the Deliver page; `:render --output out.mp4 --preset ...` renders from the line |
| `:set snap` `nosnap` `snap!` `snap?` | the options `snap`, `linked`, `audioscrub`, `loop` (`no` turns off, `!` or `inv` flips, `?` asks), also `vim`, `leader=`, `timeoutlen=`, `physicalkeys` |
| `:nmap` `:nunmap` `:map` `:source` | change the Vim keys (see above) |
| `:action {id or name}` | run any action of the Shortcuts tab; a bare id works too (`:edit.undo`) |
| `:export edl\|otio\|markers` `:import media\|timeline\|subtitle` | the File menu's export and import |
| `:help [topic]` `:cheat` | open the guide (a topic is searched); the Vim keys page |
| `:noh` `:play` | forget the search; play / pause |
| `:marker add --at 120` | any Kinoklip command, as on the command line (see [Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md)) |

Commands can be shortened as in Vim (`:w`, `:bn`, `:pa edit`, `:red`). `:set novim` switches Vim mode
off, so you can never get stuck.

## Pickers

Space `f` opens a list with a filter field (Telescope's way). Type to narrow it: every word must match
as letters in order (`cutpg` finds *Cut Page*), a capital letter makes that word case sensitive.
**Ctrl+J** and **Ctrl+K** (or Down and Up, Ctrl+N and Ctrl+P) move, **Enter** chooses, **Esc** closes.

| Keys | Lists | Enter |
|---|---|---|
| `Space` `f` `f` | the media of the project | shows it in the Source viewer |
| `Space` `f` `c` | every command, with its id and key | runs it |
| `Space` `f` `e` | effects, transitions, titles and generators | applies it to the clip under the cursor; a transition goes to the edit point at the cursor (the clip that starts there, else the one that ends there). **Alt+Enter** only shows what it is |
| `Space` `f` `t` | the timelines | opens it |
| `Space` `f` `m` | the markers | goes to it |
| `Space` `f` `d` | the bins | shows the bin in the Media Pool |
| `Space` `f` `h` | the pages of the guide | opens it |
| `Space` `f` `k` | the Vim keys | runs its command (a motion says which keys to type) |
| `Space` `f` `p` | the Project Manager | |

## If you used the Vim keyboard profile

Versions 1.2 to 1.4 had a "Vim" keyboard profile. It is gone: on the first start Kinoklip turns Vim
mode on instead and switches the profile to Resolve, keeping the keys you had changed. The commands
that were moved to **Alt** + their key are back on their own keys, and work in INSERT mode. In NORMAL
mode, `i` and `a` now switch to INSERT (as in Vim); the source edits are `Space` `s` `i` / `o` / `a`,
and play / pause is `Space` `Space` (plain Space plays in INSERT). Four keys changed to follow Vim:
`gg` and `G` go to the first and last **track** (`0` and `$` are still the start and end of the
timeline), `H` and `L` switch to the previous and next timeline tab (`{` and `}` are one second),
`j` and `k` move the cursor track over video and audio, and `n` and `N` repeat a search
(`]]` and `[[` are still the markers).

## What is different from Vim

- `d` is a ripple delete and `c` leaves a gap; `p` inserts at the playhead and `P` overwrites.
- The cursor sits on a **frame**, so `$` is the end of the whole timeline (not of the track) and
  `dj` (a motion between tracks) works on the clips under the cursor frame, not on whole tracks.
- The windows are the **panels**: `Ctrl+W` `o` hides the other panels (it does not close them), `Ctrl+W` `c` hides this one, `Ctrl+W` `w` wraps from the last panel to the first. `gt` counts pages (Neovim: tab pages) and `{n}gt` is page n; `2gT` goes two back, wrapping round, as in Neovim.
- In lists `yy` copies the *names* of the rows as text, `p` is the panel's own paste, and `dd` presses Delete in the view (the panel decides what is deleted). `Ctrl+D` / `Ctrl+U` move half the *visible* rows.
- On the `:` line `:q` quits the program (Vim closes a window), `:e` opens a timeline, and a number goes to a **frame** (Vim: a line). `:q!` quits without asking. `Ctrl+N` / `Ctrl+P` step through completions and, when none are listed, through the history, as in Neovim. There are no ranges (`:%s`), no `:!`.
- Keys that Neovim has for text and Kinoklip has no use for (`f`, `t`, `%`, `*`, `q` macros,
  `"Ayy` appending, the numbered registers) do nothing.
- `p` in VISUAL replaces the selection but does not put the removed clips into a register.
- In the Inspector `Ctrl+A` and `Ctrl+X` step a value (they are not "add" and "subtract" on text), and `x` toggles, because `Space` is the leader.
- `/` finds marker and clip names, not text; `?` is not bound.
- `J`, `r`, `s` and `~` are Kinoklip's edits (join, replace, split, enable), not Vim's.

Your choice and your changes to the keys are saved in your settings, so
[they move with you](move-your-settings-to-another-computer.md).
