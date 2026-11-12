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
focus; that panel gets an accent frame. For now the keys act on the **timeline and the viewers**;
click in one to use them. Everywhere else the profile's shortcuts work as before.

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
| `Space` `f` `p` / `n` / `x` | Project Manager / show the Media Pool / close the timeline tab |
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

`:` opens a line in the page bar row, in place of the mode line.

- `:w` saves the project, `:q` closes the window (asking about unsaved changes as usual), `:wq` does both.
- `:set novim` switches Vim mode off, so you can never get stuck.
- Any Kinoklip command works too, written as on the command line: `:marker add --at 120 --name Intro`.
  See [Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md).
- `:nmap`, `:nunmap`, `:map`, `:source` and `:set` change the Vim keys (see above).
- **Up** and **Down** bring back what you typed before. There is no Tab completion.

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
- Keys that Neovim has for text and Kinoklip has no use for (`f`, `t`, `%`, `*`, `q` macros,
  `"Ayy` appending, the numbered registers) do nothing.
- `p` in VISUAL replaces the selection but does not put the removed clips into a register.
- `/` finds marker and clip names, not text; `?` is not bound.
- `J`, `r`, `s` and `~` are Kinoklip's edits (join, replace, split, enable), not Vim's.

Your choice and your changes to the keys are saved in your settings, so
[they move with you](move-your-settings-to-another-computer.md).
