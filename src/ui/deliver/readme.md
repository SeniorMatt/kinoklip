# src/ui/deliver — Deliver (render) page

Owned by the export work. Depends on `src/export` (settings, presets, render queue), `render`
(preview Compositor), `ui/common`, `ui/viewer`.

```
┌ Render Settings ───(…)─┬──────── Timeline viewer ────────┬ Render Queue ──────────┐
│ ‹ [tile][tile][tile] › │                                 │ ┌ job card ──────────┐ │
│ YouTube 1080p          │   frame under the playhead      │ │ name  [x][dir][-]  │ │
│ Filename [_________]   │   (preview Compositor while no  │ │ file · size · N fr │ │
│ Location [_____][Browse]   PlaybackEngine drives it)    │ │ ███████░░  42% ETA │ │
│ Render (o)Single ( )Individual (disabled)                │ └────────────────────┘ │
│ [Video][Audio][File]   │                                 │                        │
│  Format / Codec / Encoder / Profile / Resolution / Frame │                        │
│  rate / Quality / Speed / Key frames / Reordering / tags │                        │
│ Render [Entire ▾] [Add to Render Queue]                  │ [Render All][Stop][Clear]
├────────────────────────┴─────────────────────────────────┴────────────────────────┤
│ mini timeline: lanes per track, render range highlighted, playhead (click/drag)   │
└───────────────────────────────────────────────────────────────────────────────────┘
```

| File | What |
|---|---|
| `DeliverPage.*` | the page (`kk::Page`, id `deliver`): splitters, owns `ExportPresets` + `RenderQueue`, debounced playhead preview through its own `Compositor` (one request in flight, rendered at the viewer size), hardware encoder detection on first show (worker thread), "Add to Render Queue" with overwrite confirmation |
| `RenderSettingsPanel.*` | Resolve's Render Settings: preset row, filename + location, single / individual clips, Video / Audio / File tabs, render range; a view of one `ExportSettings` that is normalised after every change so invalid combinations cannot be chosen |
| `RenderPresetRow.*` | the preset tiles (icon over a two-line name, elided with a tooltip), scrolled with arrow buttons instead of a scroll bar |
| `RenderQueuePanel.*` | one card per job (status, progress bar, fps, ETA, edit - loads the job into Render Settings with Update Job, spec 45.1 -, cancel, reveal output folder, remove), Render All / Stop / Clear Finished |
| `RenderFinishNotifier.*` | the "render finished" system notification (spec 45.5, our own, like HandBrake's): one per queue run, "Render complete" / "Render finished with errors: 1 of 3 failed", Open Folder and Play buttons, none after a user Stop or with the Preferences option off (45.6); taskbar flash when the window is inactive. Sends through `platform/DesktopNotifications` |
| `RenderRangeBar.*` | mini timeline (clips per track, range, playhead; click/drag seeks) |

Behaviour notes:
* Jobs snapshot the timeline when they are queued; the user can keep editing (other pages)
  while the queue renders in the background, one job at a time.
* Applying a preset keeps the file name, location and render range.
* Frame rate is always the timeline rate (no retiming); resolution is timeline, a standard
  size or custom.
* Quality is Resolve's "Automatic [Best / High / Medium / Low / Custom]" or "Restrict to N
  Kb/s"; the levels map to per-codec CRF / CQ values (`RenderSettingsPanel::qualityValue()`).
* Key frames (automatic = every 2 s, or every N frames) and frame reordering (B-frames) go to
  `VideoExportSettings::keyframeInterval` / `frameReordering`.
* Disabled with a tooltip because the engine does not support them yet: individual clips,
  frame rate (always the timeline's), data levels, colour space / gamma tags (Rec.709),
  output channels (stereo), encoding profile for H.264 / H.265 / AV1 (chosen by the encoder).
* File tab: filename from a custom name or the timeline name, subfolder, unique filenames
  (`_1`, `_2` ...). Remembered in QSettings `deliver/fileNameUsesTimeline`, `deliver/useSubfolder`,
  `deliver/subfolder`, `deliver/uniqueFileNames`.
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
