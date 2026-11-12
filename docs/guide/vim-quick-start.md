# Vim mode: quick start

This is a short, step-by-step lesson for people who have never used Vim mode (or Vim itself).
It takes about fifteen minutes. When you are done, the [cheat sheet](vim-cheat-sheet.md) lists all
the keys, and [Vim for Kinoklip](edit-with-vim-keys.md) explains everything in detail.

## The idea in one minute

Normally a key does one thing: `B` picks the blade, `Delete` deletes. In Vim mode, keys are
**short words you combine**, like a tiny language:

- `w` means "the next clip", `d` means "delete". So `dw` deletes up to the next clip.
- A number in front repeats it: `3w` jumps three clips ahead, `3dd` deletes three clips.
- You never need to hold Ctrl or Shift for everyday editing, and your hands stay on the letters.

Vim mode has **modes**, and the word at the left of the page bar (the row with the page buttons)
always tells you which one you are in:

| Mode line says | What it means |
|---|---|
| **NORMAL** | Keys are commands. Pressing `d` deletes, it does not type a "d". You are here most of the time. |
| **INSERT** | Keys work like they did before Vim mode (Space plays, your usual shortcuts work). |
| **VISUAL** | You are selecting. Moving grows the selection. |
| **COMMAND** | You are typing a command after `:` |

**If you are ever lost, press Esc** (once or twice). It always brings you back to NORMAL and cancels
whatever was half typed. Nothing gets deleted by pressing Esc.

## Step 1: turn it on

1. Open **Kinoklip > Vim Mode** (or tick **Vim mode** in **Preferences > Keyboard**).
2. Look at the left end of the page bar: it now says **NORMAL**.

To turn it off later, use the same menu entry. Your normal shortcuts come back at once.

## Step 2: get something to practise on

1. Open a project and put **three or four clips** next to each other on V1 (drag them from the Media
   Pool with the mouse, as usual). Give them different names if you can.
2. Click once on the timeline so it has the keyboard focus (it gets a coloured frame).

## Step 3: move around

Vim mode has a **cursor**: it is the playhead, on one track at a time (the **cursor track**). Try
these keys and watch the playhead:

| Press | What happens |
|---|---|
| `l` / `h` | one frame right / left (hold it to keep going) |
| `}` / `{` | one second right / left |
| `w` | jump to the start of the next clip |
| `b` | jump back to the start of this clip (or the previous one) |
| `e` | jump to the end of the clip |
| `0` / `$` | the very start / the very end of the timeline |
| `j` / `k` | move the cursor down / up one track (V1, V2, A1, ...) |

Now try a number: press `2` and then `w`. You jump two clips at once.

## Step 4: your first edits

Put the cursor on the second clip (`0`, then `w`).

| Press | What happens |
|---|---|
| `s` | **split** the clip at the cursor |
| `u` | **undo** (press it again to undo more) |
| `Ctrl+R` | redo |
| `dd` | **delete the clip** under the cursor; the clips after it move left to close the gap |
| `u` | bring it back |
| `yy` then `p` | **copy** the clip, then **paste** it at the cursor |
| `.` | **repeat** the last change (after `dd`, `.` deletes the next clip too) |

Every one of these is a single undo step, so `u` always takes back exactly one thing you did.

## Step 5: combine a command with a movement

This is the heart of Vim. Take a command (`d` delete, `y` copy, `c` change) and follow it with any
movement from Step 3. The command works on everything the movement passes over:

| Press | What happens |
|---|---|
| `dw` | delete from the cursor to the next clip |
| `d}` | delete one second from the cursor |
| `d$` | delete everything from the cursor to the end |
| `y2w` | copy the next two clips |
| `d3w` | delete three clips' worth |

Undo with `u` after each one to try the next.

## Step 6: select with VISUAL

1. Press `V` (capital). The mode line says **V-LINE** and the clip under the cursor is selected.
2. Press `w` twice. The selection grows over the next clips.
3. Press `d` to delete them, or `y` to copy them. Or press **Esc** to cancel.

(Lower-case `v` selects frames instead of whole clips.)

## Step 7: the Space menu

**Space** opens a menu of everything else. Press Space and **wait half a second**: a list pops up
showing which keys can come next and what they do. You do not need to remember anything; just read
the list.

Try these:

| Press | What happens |
|---|---|
| `Space Space` | play / pause (Space alone no longer plays in NORMAL) |
| `Space m a` | add a marker |
| `Space i i` / `Space i o` | Mark In / Mark Out |
| `Space f e` | search for an effect or transition and put it on the clip under the cursor |
| `Space f f` | search your media |

In the search windows (`Space f ...`), type a few letters to filter, move with **Ctrl+J / Ctrl+K**
or the arrow keys, and press **Enter** to pick.

## Step 8: change values in the Inspector

1. With the cursor on a clip, press `Space w i`. The Inspector gets the keyboard focus and a frame
   marks one row (for example Zoom).
2. `j` / `k` move to the next / previous row.
3. `Ctrl+A` raises the value one step, `Ctrl+X` lowers it. `10 Ctrl+A` raises it ten steps.
4. To type an exact number: press `i`, type it, press **Enter**.
5. `dd` resets the row to its default.
6. **Keyframes:** move the playhead, then press `Space k k` on a row to add a keyframe there (press
   it again to remove it). `]k` / `[k` jump to the next / previous keyframe.
7. Press `Space w t` to go back to the timeline.

Text works the same way: on the Title tab, go to the text row, press `i`, type, then **Esc**.

## Step 9: move between panels

| Press | What happens |
|---|---|
| `Ctrl+H` `Ctrl+J` `Ctrl+K` `Ctrl+L` | move to the panel on the left / below / above / right |
| `Space w` + letter | jump to a panel: `t` timeline, `i` Inspector, `m` Media Pool, `e` Effects, `v` viewer, `k` Keyframes, `x` Mixer |
| `gt` / `gT` | next / previous page (Media, Cut, Edit, ...) |

In the **Media Pool** and other lists, `j` / `k` move up and down, `Enter` opens, `h` goes back up a
folder and `/` searches.

## Step 10: the command line

Press `:` and type a command, then **Enter**:

| Type | What happens |
|---|---|
| `:w` | save the project |
| `:q` | quit |
| `:120` | go to frame 120 |
| `:01:00:05:00` | go to that timecode |
| `:help` | open this guide |

Press **Tab** while typing to see suggestions.

## When you want the old keys back for a moment

Press `i`. The mode line says **INSERT** and your usual shortcuts work again (Space plays, `B`
blade, and so on). Press **Esc** to go back to NORMAL.

## Five things to remember

1. Look at the mode line: it tells you what your keys will do.
2. **Esc** always gets you out.
3. `u` undoes, one step at a time.
4. **Space**, then wait: the menu shows you what you can do.
5. Command + movement: `d` + `w` = delete to the next clip.

Next: keep the [cheat sheet](vim-cheat-sheet.md) open while you work, and read
[Vim for Kinoklip](edit-with-vim-keys.md) for the rest (registers, macros, search, the Keyframes
panel, nodes, the mixer and changing the keys).
