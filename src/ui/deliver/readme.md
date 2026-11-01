# src/ui/deliver — Deliver (render) page

Owned by the export work. Depends on `src/export` (settings, presets, render queue), `render`
(preview Compositor), `ui/common`, `ui/viewer`.

```
┌ Render Settings ───────┬──────── Timeline viewer ────────┬ Render Queue ──────────┐
│ [preset strip ........]│                                 │ ┌ job card ──────────┐ │
│ [Video][Audio][File]   │   frame under the playhead      │ │ name  [x][dir][-]  │ │
│  Format / Codec /      │   (preview Compositor while no  │ │ file · size · N fr │ │
│  Encoder / Profile /   │    PlaybackEngine drives it)    │ │ ███████░░  42% ETA │ │
│  Resolution / Quality  │                                 │ └────────────────────┘ │
│ Render: (o)Entire ( )In/Out                              │                        │
│ [Add to Render Queue]  │                                 │ [Render All][Stop][Clear]
├────────────────────────┴─────────────────────────────────┴────────────────────────┤
│ mini timeline: lanes per track, render range highlighted, playhead (click/drag)   │
└───────────────────────────────────────────────────────────────────────────────────┘
```

| File | What |
|---|---|
| `DeliverPage.*` | the page (`kk::Page`, id `deliver`): splitters, owns `ExportPresets` + `RenderQueue`, debounced playhead preview through its own `Compositor` (one request in flight, rendered at the viewer size), hardware encoder detection on first show (worker thread), "Add to Render Queue" with overwrite confirmation |
| `RenderSettingsPanel.*` | preset strip (built-in + user presets, title-bar menu to save/delete user presets), Video / Audio / File tabs, render range; a view of one `ExportSettings` that is normalised after every change so invalid combinations cannot be chosen |
| `RenderQueuePanel.*` | one card per job (status, progress bar, fps, ETA, cancel, reveal output folder, remove), Render All / Stop / Clear Finished |
| `RenderRangeBar.*` | mini timeline (clips per track, range, playhead; click/drag seeks) |

Behaviour notes:
* Jobs snapshot the timeline when they are queued; the user can keep editing (other pages)
  while the queue renders in the background, one job at a time.
* Applying a preset keeps the file name, location and render range.
* Frame rate is always the timeline rate (no retiming); resolution is timeline, a standard
  size or custom.
* The Encoder combo marks hardware backends that were not detected "(not available)"; picking
  one anyway falls back to software at render time.

## How to extend
* New setting: add the field to `ExportSettings` (+ JSON), a widget in the right tab of
  `RenderSettingsPanel` (read it in `settings()`, write it in `setSettings()`, show/hide it
  in `updateVisibility()`), and consume it in `src/export`.
* New preset: data only — `resources/export/presets.json` (see `src/export/readme.md`).
* Keep the constructor `DeliverPage(AppContext&, QWidget*)` — `MainWindow` creates it.
* When `AppContext::playback` becomes non-null, the preview compositor is not used; the
  playback engine is expected to feed the viewer.
