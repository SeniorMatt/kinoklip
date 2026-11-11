# Directory map

| Path | What |
|---|---|
| `src/main.cpp` | entry point: OpenGL setup, model objects, command-line modes, main window |
| `src/app/` | main window, pages (Media, Cut, Edit, Audio), theme (Breeze style/colour schemes/icons), project/autosave controllers, command line (`CliRunner.h`) |
| `src/app/commands/` | the command layer: named editor operations with typed parameters and JSON results (command line and MCP server) |
| `src/app/mcp/` | the Model Context Protocol server (`kinoklip mcp`): JSON-RPC handling, tools generated from the command registry, stdio loop |
| `src/app/live/` | live control of the open window: the user-only local socket server hosted by the main window (off unless Preferences > Local Control is on), its private line protocol and the client used by `kinoklip mcp` and `--live` |
| `src/platform/` | OS integration helpers (child processes get the user's environment back when bundled) |
| `src/projects/` | project library behind the Project Manager: project kinds and metadata readers, folders, metadata / poster caches, file operations |
| `src/core/` | data model (mixer model included), EditOps, snapshot undo, JSON project files, effect catalogue, preferences |
| `src/media/` | FFmpeg probe/decoders/caches (`media::probe()` is the shared contract) |
| `src/render/` | GPU compositor, CPU fallback renderer, masks, retime, audio mixer and mixer DSP (EQ, dynamics, loudness) |
| `src/matting/` | the AI tool Background Removal (spec 78): model catalogue and download, ONNX Runtime session (Robust Video Matting), the matte service (background worker, memory and disk cache) |
| `src/compositing/tracking/` | tracking for the Tracker and Planar Tracker nodes (template matching, homography maths, runner, track store and link) |
| `src/playback/` | timeline and source players, audio output, meter readings |
| `src/export/` | encoders and the render queue |
| `src/ui/common/` | AppContext, ActionRegistry + standard actions, PanelFrame, Page, icons, timecode |
| `src/ui/mediapool/` | media pool, media browser, metadata panels |
| `src/ui/timeline/` | timeline panel, tabs, stacked timelines, level lines |
| `src/ui/viewer/` | source/timeline viewers (OpenGL surface + raster fallback) |
| `src/ui/inspector/` | clip inspector |
| `src/ui/effects/` | effects library, its apply actions and Live Preview (hovered entry shown in the timeline viewer) |
| `src/ui/editindex/` | Edit Index panel: events and markers of the timeline |
| `src/ui/cut/` | Cut page widgets: viewer modes and source tape, overview timeline, smart edit tool bar, fast review |
| `src/ui/audio/` | Audio page widgets: mixer strips, faders, meters, loudness panel, EQ and dynamics windows |
| `src/ui/deliver/` | Deliver (render) page |
| `src/ui/projects/` | Project Manager window: cards and list, folders, New Project dialog |
| `src/ui/dialogs/` | project settings, clip attributes and other dialogs |
| `src/ui/keyframes/` | keyframes panel, dope sheet and curve editor |
| `src/ui/help/` | in-app help: F1, "?" buttons, Help > Guide, links to the user guide (`docs/guide/`) |
| `resources/` | icons, default keymap and other editors' keyboard presets, KDE colour schemes (fallback build), `.qrc` |
| `tests/<area>/test_*.cpp` | QtTest unit/smoke tests (each file is its own executable) |
| `docs/` | roadmap, behaviour spec, keymap, coding style, release notes, feedback; `docs/guide/` the user guide (articles, annotated screenshots, `index.json` for the in-app help) |
| `packaging/` | Linux AppImage (AppRun launcher, desktop entry, AppStream metadata); Windows and macOS placeholders |
| `scripts/` | packaging, formatting, keymap generators (`keymap-resolve-windows.py`, `keymap-presets.py`), test media generators, user guide and readme screenshots (`guide_screenshots.py`, `readme_screenshots.py`) |

Each module directory has a `readme.md` with its responsibilities and a **How to extend** section.
