# src/app/commands - the command layer

Named editor operations that work on a project file, without a window. Each command has a name
(`clip.add`), typed parameters, a description, and returns a JSON object (plus a short sentence
for people) or an error with a stable code. Front ends only translate: the command line
(`../CliRunner.h`) is one, an MCP server will be another and builds its tool list from
`CommandRegistry::describeAll()` (`kinoklip commands --json`). No editing logic lives in a front
end, and a command never edits a timeline in a way the UI cannot: they call `editops::*`,
`Project` and the same helpers as the timeline controller.

## Files

| File | What |
|---|---|
| `Command.h/.cpp` | `CommandSpec` / `ParamSpec` (name, type, description, required, default, choices), `CommandArgs` (typed access, `time()` through the timecode helpers), `CommandResult`, `CommandRegistry` (`builtin()`, `describe*()`, `execute()`). `execute()` is the one entry: checks arguments, loads the project (`ProjectUse`), selects `--timeline`, runs the handler, saves when the command modifies. Adds the universal parameters `project`, `output`, `timeline`, `force` |
| `CommandSupport.h/.cpp` | track names (V1, A2), JSON of clips and timelines, id lists, time reading, frame-rate rationals |
| `ProjectCommands.cpp` | `version`, `project.new`, `project.info` |
| `MediaCommands.cpp` | `media.import`, `media.list`, `media.relink` |
| `TimelineCommands.cpp` | `timeline.list`, `timeline.new`, `timeline.select` |
| `ClipCommands.cpp` | `clip.list`, `clip.add`, `clip.move`, `clip.trim`, `clip.split`, `clip.delete`, `clip.set` |
| `MarkerCommands.cpp` | `marker.add`, `marker.list`, `marker.delete` |
| `TransitionCommands.cpp` | `transition.add` |
| `ExportCommands.cpp` | `export.otio`, `export.edl`, `import.otio`, `render` (uses `export/RenderCommand.h` `renderProject()`) |

## Rules

- A handler gets a loaded `Project` and checked `CommandArgs`, returns `CommandResult::success(data, text)` or `fail("code", message)`. Make every edit with `timelineedit::runEdit()` (one undo step, cancelled when nothing changed) and report an unusable request as an error, never as a silent no-op.
- Times are timeline frames or timecode; read them with `readTime()`. Ids: clips by `ClipData::id`, media by `MediaItem::id`, timelines by `TimelineData::uid` or name, tracks as `V1` / `A2`.
- Error codes are part of the interface (`unknown_clip`, `out_of_range`, `bad_time`, ...); do not rename them.
- Nothing here may use widgets, dialogs or the network. The command-line process is a QGuiApplication on the offscreen platform.

## How to extend

1. Add the handler and its `registry.add({{name, description, params, ProjectUse, usesTimeline}, handler})` in the fitting `*Commands.cpp` (or a new file with a `register...Commands()` called from `CommandRegistry::builtin()`).
2. Describe every parameter (the description is what the user and the language model read).
3. Add a case to `tests/app/test_commands.cpp` (result, project state read back, error cases) and a row to docs/behavior-spec.md section 52; list it in the guide page.
