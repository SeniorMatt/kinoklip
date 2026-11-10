# Edit with vim keys

If you live in [Vim](https://www.vim.org) or [Neovim](https://neovim.io), Kinoklip has a keyboard
preset that edits the timeline with the keys you already know: `5l` steps five frames, `w` jumps to
the next cut, `dd` removes the clip under the playhead, `u` undoes.

## Switch it on

1. Choose **Kinoklip > Keyboard Customization** (**Ctrl+Alt+K**).
2. Open **Keyboard profile** and pick **Vim**, then click **OK**.

The timeline's tool bar now shows the mode: **NORMAL**.

The vim keys work only while the **timeline or a viewer has the keyboard focus** (click on the
timeline). In a text field, the Inspector, a dialog or the title editor, keys type text as they
always do: typing `dd` in a name field types two letters. Every other keyboard profile works as
before.

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
| `u` `Ctrl+R` | undo / redo |
| `i` `o` `a` | insert / overwrite / append the clip from the Source viewer |
| `s` | split at the playhead |
| `>` `<` | nudge the selected clips a frame right / left |
| `M` | add a marker |
| `v` | visual mode |
| `:` | command line |

Each edit is one step, so one `u` takes back one `dd`.

## Visual mode

Press `v` to start selecting clips. Move with `l`, `w`, `G` and so on: every clip between where you
started and the playhead is selected, on all tracks. Then `d` or `x` ripple deletes them, `X`
deletes them and leaves a gap, `y` copies them. **Esc** or `v` goes back to normal mode.

## The command line

`:` opens a line at the bottom of the window.

- `:w` saves the project, `:q` closes the window (asking about unsaved changes as usual), `:wq` does both.
- Any Kinoklip command works too, written as on the command line: `:marker add --at 120 --name Intro`.
  See [Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md).
- **Up** and **Down** bring back what you typed before. There is no Tab completion.

## What is not there

`.` (repeat) and `/` (search) have no counterpart on a timeline, and `d` or `y` followed by a
motion (`dw`) is not supported: those keys do nothing instead of doing something odd. Keys that
have no meaning on a timeline are swallowed in the same way. The commands that used a plain letter
(Mark In `I`, Blade `B`, ...) moved to **Alt** plus the same key where that is free; the
[full table](../keymaps/vim.md) lists them all.

Your choice, your changes to the keys and the Vim profile itself are saved in your settings, so
[they move with you](move-your-settings-to-another-computer.md).
