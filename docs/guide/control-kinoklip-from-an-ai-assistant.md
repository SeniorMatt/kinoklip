# Control Kinoklip from an AI assistant (MCP)

You will connect an AI assistant (a program that talks to a language model) to Kinoklip, so that
you can ask it to build and cut a project in plain words: "put these two clips on the timeline,
cut the first 5 seconds off the start and add a dissolve between them". The assistant uses
Kinoklip's commands (the same ones as in
[Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md)) and can
look at the result by asking for a picture of a frame.

The connection uses the [Model Context Protocol](https://modelcontextprotocol.io) (MCP), an open
standard that many assistants and editors' plugin systems understand.

## What it does, and what it does not

- **Local only.** `kinoklip mcp` is a program that your assistant starts on your own computer
  and talks to through its standard input and output. It opens no network connection, listens on
  no port and sends nothing anywhere. Kinoklip itself makes no network requests. (If you switch
  on control of the open window, below, Kinoklip also listens on a socket that only your own user
  on this computer can reach; it is never reachable from the network.)
- **What leaves your computer is decided by the assistant.** If the assistant uses an online
  model, the commands it sends and what Kinoklip answers (clip names, file paths, and the
  pictures of frames it asks for) go to that model's provider. That is the assistant's
  connection, not Kinoklip's; read its privacy terms, or use an assistant with a model that runs
  on your computer.
- **By default it works on project files.** Every command reads a project file, changes it and
  saves it, like the command line. The assistant can do what the commands can, in the files your
  user can write: it can create and overwrite project files, and render, export and write
  picture files where it is told to. Tell it which folder to work in, and keep a copy of
  projects that matter. With `output` the assistant can save the changed project as a new file
  and leave the original alone.
- **By default the window is separate.** The server does not use an open Kinoklip window. If you
  have the same project open in the window, close it or reopen it after the assistant has
  changed it, and do not save over the assistant's work from the window. To let the assistant
  work in the window itself, switch on live control (next section).

## Connect an assistant

Add Kinoklip to the list of MCP servers of your assistant. Most assistants take a configuration
like this (the file and the place for it are in the assistant's own documentation):

```json
{
  "mcpServers": {
    "kinoklip": {
      "command": "kinoklip",
      "args": ["mcp"]
    }
  }
}
```

If you use the AppImage, give the full path of the file instead of `kinoklip`:

```json
{
  "mcpServers": {
    "kinoklip": {
      "command": "/home/you/Applications/Kinoklip-x86_64.AppImage",
      "args": ["mcp"]
    }
  }
}
```

The AppImage must be executable (`chmod +x`). Then restart the assistant or reload its servers.
The assistant now lists tools such as `project_new`, `clip_add` and `frame_view`. You can check
the server by hand: `kinoklip mcp` waits for messages on its input, and Ctrl+D ends it.

## Let the assistant work in the open window (live control)

By default the assistant changes project files, and you see the result when you open the file.
If you would rather watch it work, and undo what it does, switch on live control:

1. In Kinoklip open **Preferences > System > Local Control** and tick **Let local tools control
   Kinoklip (MCP)**, then Save. The option is off until you do this; while it is off, nothing
   listens and nothing can connect.
2. Open the project you want to work on.
3. Start the assistant as before. Nothing else changes in its configuration: when it starts
   `kinoklip mcp` and a window accepts local control, the tools work on the open project. The
   tool `session_info` tells the assistant which mode it is in and which project is open (you
   can ask it).

What is different in live mode:

- **You see every change at once** in the window, and each tool call is **one step** in
  Edit > Undo ("Assistant: clip add"), however many things it changed. Undo takes it back. A call
  that fails changes nothing.
- **Nothing is saved until you save** (or the assistant calls `project_save`, which does what
  File > Save Project does). The project needs a file for that; save it once yourself first.
- **`project` can be left out.** If the assistant gives it, it must be the project that is open
  (otherwise it gets an error that names the open project), so it cannot change another file by
  mistake. `output` does not exist in live mode. `project_new` still makes a new file and
  does not touch the window.
- **While you are in the middle of dragging or typing in a field** the assistant is told that the
  window is busy and tries again; it never takes your gesture over.
- **Renders and frame pictures** are made from a copy of the project in the background, so the
  window stays usable; they are not undo steps.
- **While an assistant is connected** the bar at the top of the window shows "Assistant
  connected".
- **If no window accepts**, or the option is off, `kinoklip mcp` works on files exactly as
  before. `kinoklip mcp --file` always works on files. On the command line, add `--live` to any
  command to run it in the open window.

What live control does not do: it cannot do anything the commands cannot (it does not click
buttons or open dialogs), it is limited to your own user account (other users of the computer
cannot connect: the socket is only for you), it never listens on the network, and it only
works while Kinoklip is open with the option on. Only one Kinoklip window at a time offers it.
Switch it off in the same place at any time.

## Ask for something

Be concrete about files. For example:

- "Make a project demo.kkproj in ~/Videos/ai at 1080p 25 fps, import ~/Videos/holiday.mp4, put
  seconds 10 to 20 of it on the timeline and show me the first frame."
- "List the clips of demo.kkproj, split the first at 00:00:04:00, and add a Cross Dissolve at
  the cut."
- "Which transitions does Kinoklip have with 'wipe' in the name?"
- "Add a title 'Summer 2026' at the start of the timeline for three seconds, and fade the first
  clip in from black over one second." (a title, and keyframes on the clip's opacity)
- "Make the second clip slightly blurred, turn it off for now, and mute the music track."
- "Render demo.kkproj to ~/Videos/ai/demo.mp4 for YouTube."

## The tools

Every command of Kinoklip is a tool; the name is the command with an underscore: `clip add` is
`clip_add`. `kinoklip commands` lists them all. Besides the editing commands there are four that
help the assistant to see its work:

| Tool | What it gives |
|---|---|
| `project_info`, `clip_list`, `media_list`, `timeline_list` | The state of the project: settings, timelines, clips with their times, media and which files are offline. |
| `frame_view` | A picture of one frame of the timeline, at most 1024 pixels wide unless asked otherwise, so the assistant can look at what it built. Needs an assistant that can read pictures. |
| `frame_export` | One frame as a PNG file (also on the command line: `kinoklip frame export`). |
| `transition_list`, `effect_list` | The ids of the transitions, effects, generators and titles (for example the `type` of `transition_add`), with the settings of one when you give its `id`. |
| `settings_export` | Write all of Kinoklip's settings to a `.kinoklip-settings` file. There is no tool to import settings: replacing your settings is something only you can do (Preferences > Import and Export, or `kinoklip settings import` in a terminal). |
| `session_info` | Whether the tools change project files (`file`) or the open window (`live`), and which project is open. |

There is no tool to export the log file either (it can name your folders): that is **Help > Export Log File...**, or `kinoklip log export` in a terminal.

For editing beyond cuts the assistant has, besides the cutting tools:

| Tools | What they do |
|---|---|
| `effect_add`, `effect_stack`, `effect_set`, `effect_remove`, `effect_enable` | Put an effect on a clip, list a clip's effects, change their settings by key (checked against the range the Inspector uses), take them off, switch them on or off. |
| `transition_add`, `transition_set`, `transition_remove` | A transition at a clip's start or end; its length, alignment and settings. |
| `title_add`, `title_set`, `generator_add` | A text title (text, font, size, colour, bold, italic, alignment, position), a solid colour, gradient, bars or adjustment clip, on a track at a time for a length. |
| `keyframe_set`, `keyframe_list`, `keyframe_delete` | Animate a number of a clip or an effect (opacity, position, zoom, rotation, crop, volume, an effect's setting) over time. |
| `clip_set` | Speed, volume, pan, on or off, name, zoom, position, rotation, opacity and crop of a clip. |
| `track_list`, `track_add`, `track_set` | Add tracks; mute, lock, solo, enable or rename a track. |

Times are frames (`125`) or timecode (`00:00:05:10`) counted from the start of the timeline. When
a tool fails, the assistant gets the reason and a short code (for example `unknown_clip` or
`out_of_range`) and can try again. A long render reports its progress to assistants that ask for
it.

## If something goes wrong

- **The assistant does not list Kinoklip.** Check that `kinoklip` (or the AppImage path) runs in
  a terminal, and that the configuration is valid JSON. Restart the assistant.
- **"project file ... does not exist".** The path is wrong. Ask the assistant to use full paths,
  or tell it the folder you work in.
- **The assistant cannot see pictures.** `frame_view` returns an image; use `frame_export` and
  open the file yourself, or pick an assistant that reads images.
- **My window shows the old project.** Unless live control is on, the server changes the file,
  not the open window. Reopen the project from File > Recent Projects, or switch on live
  control (see above).
- **Live control is on but the assistant still changes files.** Open Kinoklip first and tick the
  option in Preferences > System > Local Control, then start (or restart) the assistant; ask it
  for `session_info`. If a second Kinoklip window is open, only the first one offers control.
- **"wrong_project".** In live mode the assistant named a file that is not the open project.
  Tell it to leave `project` out, or open that project in the window.
- **"window_busy".** You are in the middle of an edit in the window (a drag, a typed value).
  Finish it; the assistant tries again.
