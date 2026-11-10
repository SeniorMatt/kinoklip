# src/app/commands - the command layer

Named editor operations that work on a project file, without a window. Each command has a name
(`clip.add`), typed parameters, a description, and returns a JSON object (plus a short sentence
for people) or an error with a stable code. Front ends only translate: the command line
(`../CliRunner.h`) is one, the MCP server (`../mcp/`) is another and builds its tool list from
`CommandRegistry::describeAll()` (`kinoklip commands --json`). No editing logic lives in a front
end, and a command never edits a timeline in a way the UI cannot: they call `editops::*`,
`Project` and the same helpers as the timeline controller.

## Files

| File | What |
|---|---|
| `Command.h/.cpp` | `CommandSpec` / `ParamSpec` (name, type, description, required, default, choices), `CommandArgs` (typed access, `time()` through the timecode helpers), `CommandResult`, `CommandRegistry` (`builtin()`, `describe*()`, `execute()`). `execute()` is the one entry: checks arguments, loads the project (`ProjectUse`), selects `--timeline`, runs the handler, saves when the command modifies. Adds the universal parameters `project`, `output`, `timeline`, `force` |
| `CommandSupport.h/.cpp` | track names (V1, A2), JSON of clips and timelines, id lists, time reading, frame-rate rationals |
| `ProjectCommands.cpp` | `version`, `project.new`, `project.info`, `project.save`, `session.info` |
| `MediaCommands.cpp` | `media.import`, `media.list`, `media.relink` |
| `TimelineCommands.cpp` | `timeline.list`, `timeline.new`, `timeline.select` |
| `ClipCommands.cpp` | `clip.list`, `clip.add`, `clip.move`, `clip.trim`, `clip.split`, `clip.delete`, `clip.set` |
| `MarkerCommands.cpp` | `marker.add`, `marker.list`, `marker.delete` |
| `TransitionCommands.cpp` | `transition.add`, `transition.set`, `transition.remove` |
| `FrameCommands.cpp` | `frame.export` (a timeline frame to a PNG file), `frame.view` (the picture in `CommandResult::image`; MCP sends it as image content) through `renderFrameImage()` |
| `EffectCommands.cpp` | `transition.list`, `effect.list`: the ids of the effect catalogue (`EffectRegistry`) |
| `ClipEffectCommands.cpp` | `effect.add`, `effect.stack`, `effect.set`, `effect.remove`, `effect.enable`: the effects of a clip, added with `effectactions::applyEffect()` (the Effects library's function) |
| `GeneratorCommands.cpp` | `generator.add`, `title.add`, `title.set`: generated and title clips, made and placed with `effectactions::makeGeneratorClip()` / `addGeneratorClip()` |
| `KeyframeCommands.cpp` | `keyframe.set`, `keyframe.list`, `keyframe.delete` through `paramaccess` and `keyframeops` (the Inspector's code) |
| `TrackCommands.cpp` | `track.list`, `track.add`, `track.set` |
| `ParamSupport.h/.cpp` | `key=value` settings checked against a `ParamDescriptor` (range, choices, colour, file) and written through `paramaccess`; finding a clip parameter by key |
| `ExportCommands.cpp` | `export.otio`, `export.edl`, `import.otio`, `render` (uses `export/RenderCommand.h` `renderProject()`) |

## File mode and live mode

`CommandRegistry::execute()` is file mode: it loads the project file, runs the handler and saves.
`CommandRegistry::executeLive()` runs the same handler against the project of an open window
(`LiveTarget`; hosted by `../live/`): no load, no save, `project` optional and checked against the open
project, the whole call wrapped in one `Project::beginEdit()` / `endEdit()` (an undo step; a failed
call is cancelled). Handlers therefore never load, save or open an edit around a whole command
themselves, and ask `ctx.live` only for the few things that differ (`project.save`, `session.info`).
`CommandSpec::background` marks the long jobs (`render`, `frame.*`): the window runs them on a copy of
the project (`executeOnSnapshot()`) in a worker thread. `CommandSpec::liveUndoStep = false` for a command that
must not run inside an open edit.

## Rules

- A handler gets a loaded `Project` and checked `CommandArgs`, returns `CommandResult::success(data, text)` or `fail("code", message)`. Make every edit with `timelineedit::runEdit()` (one undo step, cancelled when nothing changed) and report an unusable request as an error, never as a silent no-op.
- Times are timeline frames or timecode; read them with `readTime()`. Ids: clips by `ClipData::id`, media by `MediaItem::id`, timelines by `TimelineData::uid` or name, tracks as `V1` / `A2`.
- Error codes are part of the interface (`unknown_clip`, `out_of_range`, `bad_time`, ...); do not rename them.
- Nothing here may use widgets, dialogs or the network. The command-line process is a QGuiApplication on the offscreen platform.

## How to extend

1. Add the handler and its `registry.add({{name, description, params, ProjectUse, usesTimeline}, handler})` in the fitting `*Commands.cpp` (or a new file with a `register...Commands()` called from `CommandRegistry::builtin()`).
2. Describe every parameter (the description is what the user and the language model read).
3. Add a case to `tests/app/test_commands.cpp` (result, project state read back, error cases) and a row to docs/behavior-spec.md section 52; list it in the guide page.
