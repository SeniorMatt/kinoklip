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
  no port and sends nothing anywhere. Kinoklip itself makes no network requests.
- **What leaves your computer is decided by the assistant.** If the assistant uses an online
  model, the commands it sends and what Kinoklip answers (clip names, file paths, and the
  pictures of frames it asks for) go to that model's provider. That is the assistant's
  connection, not Kinoklip's; read its privacy terms, or use an assistant with a model that runs
  on your computer.
- **It works on project files.** Every command reads a project file, changes it and saves it,
  like the command line. The assistant can do what the commands can, in the files your user can
  write: it can create and overwrite project files, and render, export and write picture files
  where it is told to. Tell it which folder to work in, and keep a copy of projects that matter.
  With `output` the assistant can save the changed project as a new file and leave the original
  alone.
- **The window is separate.** The server does not use an open Kinoklip window. If you have the
  same project open in the window, close it or reopen it after the assistant has changed it,
  and do not save over the assistant's work from the window.

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
- **My window shows the old project.** The server changes the file, not the open window. Reopen
  the project from File > Recent Projects.
