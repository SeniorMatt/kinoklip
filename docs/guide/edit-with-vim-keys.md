# Edit with vim keys

If you live in Vim or Neovim, Kinoklip has a **Vim mode** that edits the timeline with the keys you
already know: `5l` steps five frames, `w` jumps to the next cut, `dd` removes the clip under the
playhead, `u` undoes. It is an option, off by default, and it works with every keyboard profile
(Resolve, Premiere Pro, ...): it does not replace the profile.

## Switch it on

Either tick **Vim mode** in **Preferences > Keyboard**, or choose **Kinoklip > Vim Mode**. The mode
line appears at the left of the page bar, on every page:

- **NORMAL**: keys are Vim commands. Nothing is typed.
- **VISUAL**: `v` starts selecting; motions extend the selection.
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

## The keys

Type a count first to repeat: `5l`, `3w`, `2dd`, `3u`. **Esc** drops what you have typed.

| Keys | What it does |
|---|---|
| `h` `l` | previous / next frame |
| `H` `L` | one second back / forward |
| `w` `b` | next / previous cut |
| `gg` `0` | start of the timeline |
| `G` `$` | end of the timeline |
| `n` `N` | next / previous marker |
| `m` + letter, `'` + letter | remember the playhead, jump back to it |
| `zi` `zo` `zf` | zoom in, out, to fit |
| `j` `k` | target video track down / up |
| `dd` | ripple delete the clip under the playhead (`3dd`: and the next two) |
| `x` | ripple delete the selected clips |
| `X` | delete the selected clips and leave a gap |
| `yy` | copy the clip under the playhead |
| `p` `P` | paste at the playhead, inserting / overwriting |
| `u` | undo (redo is your profile's key, Ctrl+Shift+Z in Resolve) |
| `gi` `go` `ga` | insert / overwrite / append the clip from the Source viewer |
| `s` | split at the playhead |
| `>` `<` | nudge the selected clips a frame right / left |
| `M` | add a marker |
| `i` `a` | INSERT mode |
| `v` | visual mode |
| `:` | command line |

Each edit is one step, so one `u` takes back one `dd`.

## Visual mode

Press `v` to start selecting clips. Move with `l`, `w`, `G` and so on: every clip between where you
started and the playhead is selected, on all tracks. Then `d` or `x` ripple deletes them, `X`
deletes them and leaves a gap, `y` copies them. **Esc** or `v` goes back to normal mode.

## The command line

`:` opens a line in the page bar row, in place of the mode line.

- `:w` saves the project, `:q` closes the window (asking about unsaved changes as usual), `:wq` does both.
- `:set novim` switches Vim mode off, so you can never get stuck.
- Any Kinoklip command works too, written as on the command line: `:marker add --at 120 --name Intro`.
  See [Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md).
- **Up** and **Down** bring back what you typed before. There is no Tab completion.

## If you used the Vim keyboard profile

Versions 1.2 to 1.4 had a "Vim" keyboard profile. It is gone: on the first start Kinoklip turns Vim
mode on instead and switches the profile to Resolve, keeping the keys you had changed. The commands
that were moved to **Alt** + their key are back on their own keys, and work in INSERT mode. In NORMAL
mode, `i` and `a` now switch to INSERT (as in Vim); the source edits are `gi`, `go` and `ga`.

## What is not there

`.` (repeat) and `/` (search) have no counterpart on a timeline yet, and `d` or `y` followed by a
motion (`dw`) is not supported: those keys do nothing instead of doing something odd.

Your choice and your changes to the keys are saved in your settings, so
[they move with you](move-your-settings-to-another-computer.md).
