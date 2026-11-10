# Control Kinoklip from the command line

You will make a project, put a clip on its timeline, cut it and render it, all from a terminal
or a script, without opening the window. Every command works on a project file, saves its
change into that file, and can print its result as JSON for other programs.

Nothing is sent anywhere: the commands only read and write the files you name.

## The idea

```
kinoklip <command> --option value --option value
```

- `kinoklip` with a project name (or nothing) opens the window, as always.
- `kinoklip commands` lists the commands. `kinoklip commands --json` lists them with every option, for programs.
- `kinoklip help clip add` shows the options of one command.
- Add `--json` to any command to get the result as one line of JSON instead of a sentence.
- A command that works returns exit code 0. A command that fails prints the reason on the error output and returns a number above 0 (1: it could not do it; 2: the command line is wrong).

Commands that change a project take `--project file.kkproj`. Add `--output other.kkproj` to
keep the original and save the changed copy there.

## Make a first cut

```
kinoklip project new --project demo.kkproj --width 1920 --height 1080 --fps 25
kinoklip media import --project demo.kkproj --file holiday.mp4
kinoklip media list --project demo.kkproj
kinoklip clip add --project demo.kkproj --media holiday.mp4 --source-in 00:00:10:00 --source-out 00:00:20:00
kinoklip clip list --project demo.kkproj
kinoklip clip split --project demo.kkproj --at 00:00:04:00
kinoklip clip trim --project demo.kkproj --clip 3 --edge tail --delta -12
kinoklip render --project demo.kkproj --output demo.mp4
```

The `--clip` number comes from `clip list` (the first clip you add is not always number 1). `clip add` appends after the last clip on the track. Give `--at` to put it somewhere else, and
`--insert` to push later clips to the right instead of covering them. A clip with sound brings
its audio along on the audio track with the same number, linked to the picture.

## Times

A time is a number of frames (`125`) or a timecode (`00:00:05:10`), counted from the start of
the timeline. A minus sign makes it negative (`--delta -8` moves an edge 8 frames earlier).

## The commands

| Command | What it does |
|---|---|
| `version` | Shows the version. |
| `project new`, `project info` | Create a project; show its settings, timelines and media. |
| `media import`, `media list`, `media relink` | Add files; list them (and which are offline); find moved files again. |
| `timeline list`, `timeline new`, `timeline select` | List timelines; make an empty one; choose the active one. |
| `clip list`, `clip add`, `clip move`, `clip trim`, `clip split`, `clip delete`, `clip set` | Look at and edit the clips of a timeline (speed, volume, pan, on or off, name, zoom, position, rotation, opacity, crop with `clip set`). |
| `marker add`, `marker list`, `marker delete` | Timeline markers. |
| `transition add`, `transition set`, `transition remove` | A dissolve (or an audio cross fade) at the start or end of a clip; change its length, alignment or settings; take it off. |
| `effect add`, `effect stack`, `effect set`, `effect remove`, `effect enable` | Put an effect on a clip (`--set radius=12`), list a clip's effects, change their settings by key (checked against the range the Inspector uses), remove them, switch them on or off (`--enabled false`). |
| `title add`, `title set`, `generator add` | A text title (`--text`, `--size`, `--color`, `--font`, `--bold`, `--align`) or a generated clip (solid colour, gradient, bars, adjustment clip) at a time (`--at`), for a length (`--duration`), on a track (`--track V2`). |
| `keyframe set`, `keyframe list`, `keyframe delete` | Animate a number of a clip or an effect: `keyframe set --clip 3 --param opacity --at 00:00:01:00 --value 100`. |
| `track list`, `track add`, `track set` | The tracks and their state; add one; mute, lock, solo, enable or rename one. |
| `session info`, `project save` | Which mode the commands run in (see `--live` below) and which project is open; save the open project (live mode). |
| `export otio`, `export edl`, `import otio` | Exchange a timeline with other editors (see [Move a timeline to or from another editor](move-a-timeline-to-another-editor.md)). |
| `frame export`, `frame view` | Render one frame of the timeline to a PNG file (`--at`, `--file`, `--width`); `frame view` hands the picture to a program that can show it (the AI assistant server below). |
| `transition list`, `effect list` | The ids of the transitions, effects, generators and titles, for `transition add --type`; add `--id` for the settings of one. |
| `render` | Render to a file with a preset from the Deliver page, for example `--preset youtube_1080p`; progress is shown on the error output. |

Commands that look at a timeline use the active one. Add `--timeline "Name"` (or its number
from `timeline list`) to use another for that one command.

Clips are named by the number in `clip list`. Tracks are written like on the timeline: `V1`,
`A2`. Moving, trimming, splitting and deleting a clip also changes the clips linked to it; add
`--unlinked` to change only the one you named.

## Run a command in the open window

With **Preferences > System > Local Control > Let local tools control Kinoklip (MCP)** switched
on (it is off by default), add `--live` to any command to run it in the Kinoklip window that is
open, on the project open there, instead of on a project file:

```
kinoklip marker add --at 00:00:10:00 --name Intro --live
```

You see the change at once and Edit > Undo takes it back (one step per command); `--project` can
be left out. If no window accepts, the command says so and stops. Only your own user can connect
and nothing is opened on the network. See
[Control Kinoklip from an AI assistant](control-kinoklip-from-an-ai-assistant.md) for details.

## Use it from a script

```
kinoklip clip list --project demo.kkproj --json
```

prints

```
{"ok":true,"result":{"timeline":1,"fps":25,"clips":[{"id":1,"track":"V1","start":0, ...}]}}
```

and a failure looks like

```
{"ok":false,"error":{"code":"unknown_clip","message":"no clip with id 9 in this timeline"}}
```

The `code` stays the same between versions; the message is for people. The same commands are
the base of `kinoklip mcp`, which lets an AI assistant control Kinoklip; see
[Control Kinoklip from an AI assistant](control-kinoklip-from-an-ai-assistant.md).

## If something goes wrong

- **"unknown command".** Check the spelling with `kinoklip commands`.
- **"project file ... does not exist".** The path after `--project` is wrong; commands do not create projects except `project new`.
- **"out_of_range" when trimming.** The edge cannot move that far: the file has no more picture, the next clip is in the way, or the clip would be shorter than one frame.
- **A packed (single-file) project does not open.** The commands read ordinary project files only.
- **No window opens.** That is intended for the commands above; open the window with `kinoklip demo.kkproj`.
