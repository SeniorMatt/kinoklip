# Directory map

| Path | What |
|---|---|
| `src/main.cpp` | entry point: OpenGL setup, model objects, command-line modes, main window |
| `src/app/` | main window, pages (Media, Cut, Edit), theme (Breeze style/colour schemes/icons), project/autosave controllers, CLI |
| `src/platform/` | OS integration helpers (child processes get the user's environment back when bundled) |
| `src/core/` | data model, EditOps, snapshot undo, JSON project files, effect catalogue, preferences |
| `src/media/` | FFmpeg probe/decoders/caches (`media::probe()` is the shared contract) |
| `src/render/` | GPU compositor, CPU fallback renderer, masks, retime, audio mixer |
| `src/playback/` | timeline and source players, audio output |
| `src/export/` | encoders and the render queue |
| `src/ui/common/` | AppContext, ActionRegistry + standard actions, PanelFrame, Page, icons, timecode |
| `src/ui/mediapool/` | media pool, media browser, metadata panels |
| `src/ui/timeline/` | timeline panel, tabs, stacked timelines, level lines |
| `src/ui/viewer/` | source/timeline viewers (OpenGL surface + raster fallback) |
| `src/ui/inspector/` | clip inspector |
| `src/ui/effects/` | effects library |
| `src/ui/cut/` | Cut page widgets: viewer modes and source tape, overview timeline, smart edit tool bar, fast review |
| `src/ui/deliver/` | Deliver (render) page |
| `src/ui/dialogs/` | project settings, clip attributes and other dialogs |
| `src/ui/keyframes/` | keyframes panel, dope sheet and curve editor |
| `resources/` | icons, default keymap, KDE colour schemes (fallback build), `.qrc` |
| `tests/<area>/test_*.cpp` | QtTest unit/smoke tests (each file is its own executable) |
| `docs/` | roadmap, behaviour spec, keymap, coding style, release notes, feedback |
| `packaging/` | Linux bundle (.run, tar.zst); Windows and macOS placeholders |
| `scripts/` | packaging, formatting, keymap generator, test media generators |

Each module directory has a `readme.md` with its responsibilities and a **How to extend** section.
