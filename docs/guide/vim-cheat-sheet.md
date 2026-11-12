# Vim cheat sheet

The keys of [Vim for Kinoklip](edit-with-vim-keys.md) on one page. Turn Vim mode on in
**Preferences > Keyboard** or with **Kinoklip > Vim Mode**. `Space` is the leader. A number before a
command repeats it (`3dd`); **Esc** drops what you typed. Every key is in the generated
[table](../keymaps/vim-keys.md) and can be changed in **Keyboard Customization > Vim keys**.

## Modes

| Keys | Mode |
|---|---|
| `Esc` `Ctrl+[` | back to NORMAL (drops keys typed, leaves VISUAL and INSERT) |
| `v` `V` `Ctrl+V` | VISUAL: frames / whole clips / a block of tracks (`o` other end, `gv` again) |
| `i` `a` | INSERT: your keyboard profile's own keys work (Space plays) |
| `:` | command line |
| any text field | INSERT automatically |

## Timeline: move

| Keys | Goes to |
|---|---|
| `h` `l` | previous / next frame |
| `{` `}` | one second back / forward |
| `0` `$` `{n}\|` | start / end of the timeline, frame n |
| `w` `b` `e` `ge` | next clip start / this or previous clip start / clip end / previous clip end |
| `W` `B` `E` `gE` | the same across all tracks (edit points) |
| `^` | start of the first clip |
| `]]` `[[` `]k` `[k` | next / previous marker, keyframe |
| `j` `k` `gg` `G` `{n}G` | cursor track down / up, first, last, n-th track |
| `H` `L` | previous / next timeline tab |
| `m{a}` `'{a}` `''` | set a mark, jump to it, jump back |
| `f{c}` `F{c}` `t{c}` `T{c}` `;` `,` | find a clip by the first letter of its name, repeat, reverse |
| `/` `?` `n` `N` `*` `#` | search names forward / backward, next / previous, name under cursor |
| `zi` `zo` `zf` | zoom in / out / fit |

## Timeline: change

| Keys | Does |
|---|---|
| `d` `c` `y` + motion or object | ripple delete / delete leaving a gap and INSERT / copy |
| `dd` `cc` `yy` | the clip under the cursor (`3dd`: three clips) |
| `x` `X` `D` | ripple delete / lift the clip or selection / delete to the end of the clip |
| `p` `P` | paste inserting / overwriting (`3p`) |
| `s` `J` `~` | split, join, enable / disable the clip |
| `r` `R` | replace / ripple overwrite with the source clip |
| `>` `<` | nudge a frame right / left (`5>`) |
| `.` | repeat the last change |
| `u` `Ctrl+R` | undo / redo (`3u`) |
| `M` | add a marker |

Text objects after an operator or in VISUAL: `iw` clip, `aw` clip and gap, `iW` `aW` clip and linked
clips, `ip` `ap` run of touching clips, `im` between markers, `ir` In to Out, `it` all tracks at the
cursor.

## Registers and macros

| Keys | Does |
|---|---|
| `"a` + `y` `d` `p` | use register a; `"A` adds to it |
| `"0` `"1`-`"9` `"-` `"_` | last copy / last nine deletions / small delete / black hole |
| `"1p` then `.` `.` | paste older deletions |
| `qa` ... `q` | record a macro into a (`qA` adds to it) |
| `@a` `3@a` `@@` | play, play 3 times, play the last again |
| `@:` | run the last `:` line again |

## Leader (Space)

| Keys | Does |
|---|---|
| `Space Space` | play / pause |
| `Space s` + `i` `o` `a` `r` `f` `t` | source edits: insert, overwrite, append, replace, fit, ripple overwrite |
| `Space m` + `a` `m` `d` `c` | markers: add, modify, delete, mark clip |
| `Space i` + `i` `o` `p` | Mark In, Mark Out, play In to Out |
| `Space k` + `k` `d` `l` `i` `o` `b` `s` `h` | keyframes: add, delete, linear, ease in, ease out, both, smooth, hold |
| `Space e` + `e` `a` `d` `+` `-` | transitions: add, align, delete, longer, shorter |
| `Space t` + `v` `a` `d` | add video track, audio track, delete empty tracks |
| `Space o` + `s` `l` `a` `L` | snapping, linked selection, audio scrubbing, loop |
| `Space f` + `f` `c` `e` `t` `m` `d` `h` `k` `p` | pickers: media, commands, effects, timelines, markers, bins, help, Vim keys, projects |
| `Space w` + `m` `e` `i` `t` `v` `k` `x` `n` `d` `l` | focus a panel |
| `Space y` | copy the timecode of the cursor |
| `g` `j` `k` `l` | shuttle reverse, stop, forward |

Wait half a second after `Space`, `g`, `z`, `]` or `[` and the which-key list shows what can follow.

## Panels, windows, pages

| Keys | Does |
|---|---|
| `Ctrl+H` `Ctrl+J` `Ctrl+K` `Ctrl+L` | focus the panel left / below / above / right |
| `Ctrl+W` + `h` `j` `k` `l` `w` `W` `p` | the same / next / previous / last panel |
| `Ctrl+W` + `o` `=` `c` | zoom this panel / reset the layout / hide this panel |
| `gt` `gT` `{n}gt` | next / previous page, page n |

## Lists and trees

| Keys | Does |
|---|---|
| `j` `k` `gg` `G` `{n}G` `Ctrl+D` `Ctrl+U` | move over rows |
| `o` `l` `h` `-` | open, open or step in, close or parent, parent bin |
| `za` `zo` `zc` `zR` `zM` `zt` `zz` `zb` | folds and scrolling |
| `V` `dd` `x` `yy` `p` | select rows, delete, copy names, paste |
| `a` `r` | new bin, rename |
| `/` `n` `N` `f{c}` | search or filter rows, next, previous, find a row by its first letter |

## Inspector and other forms

| Keys | Does |
|---|---|
| `j` `k` `gg` `G` `{n}G` | move over rows |
| `Ctrl+A` `Ctrl+X` `5 Ctrl+A` | raise / lower the value (a choice cycles, a check box toggles) |
| `g Ctrl+A` `g Ctrl+X` `zl` `zh` | ten steps / a tenth of a step |
| `h` `l` | step down / up |
| `i` `a` `cw` `Enter` | type the value (INSERT), `Esc` leaves |
| `x` `Enter` | toggle, press, open |
| `dd` `yy` `p` | reset to default, copy, paste the value |
| `za` `zo` `zc` `zR` `zM` | fold sections |
| `]]` `[[` `>>` `<<` `K` | next / previous tab, move effect down / up, help for the parameter |

## Keyframes, nodes, mixer, transitions

| Where | Keys |
|---|---|
| Keyframes panel | `h` `l` key, `j` `k` row, `0` `$` first / last key, `v` `V` `U` select, `x` `dd` delete, `yy` `p` copy and paste, `o` add at the playhead, `>` `<` move, `Enter` go to key, `gs` dope sheet or curves |
| Node graphs | `h` `j` `k` `l` nearest node, `w` `b` along the signal, `a` `o` `O` add, `x` `dd` delete, `yy` `p` copy, `gb` bypass, `gc` connect (twice), `gx` `gX` disconnect, `r` rename, `Enter` controls |
| Mixer | `h` `l` strip, `j` `k` fader (half a dB), `0` reset, `i` type dB, `>` `<` pan, `m` `s` `r` mute, solo, arm, `Enter` EQ, `gd` dynamics |
| Transitions | `]t` `[t` select next / previous, `Space e +` `-` length, `Space e a` align, `Space e d` delete, `Space e e` add |

## Command line

| Keys | Does |
|---|---|
| `:w` `:q` `:wq` `:q!` | save, quit, both, quit without asking |
| `:e name` `:enew` `:bn` `:bp` `:bd` `:ls` `:b n` | timeline tabs |
| `:120` `:+10` `:$` `:00:01:10:00` | go to a frame, forward, the end, a timecode |
| `:page edit` `:panel inspector` | pages and panels by name |
| `:undo n` `:redo n` `:history` | history |
| `:set snap` `nosnap` `snap!` `snap?` | options (`snap linked audioscrub loop`) |
| `:nmap` `:nunmap` `:map` `:source` `:set leader=,` | change the keys |
| `:action id` `:marker add` | any action or Kinoklip command |
| `:help topic` `:cheat` `:noh` | help, this page, forget the search |
| `Tab` `Up` `Down` `q:` | complete, history, history as a picker |

The command line keys: `Ctrl+B` `Ctrl+E` start / end, `Ctrl+W` delete a word, `Ctrl+U` delete to the
start, `Esc` close.

## Pickers (Space f)

Type to narrow the list (letters in order), `Ctrl+J` `Ctrl+K` move, `Enter` chooses, `Esc` closes.
`Space f f` media, `c` commands, `e` effects, `t` timelines, `m` markers, `d` bins, `h` help, `k` Vim
keys, `p` projects.
