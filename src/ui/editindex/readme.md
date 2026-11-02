# src/ui/editindex — Edit Index panel

The Edit page's Edit Index (spec 18.8, 18.9): a table of the active timeline's events or
markers, like Resolve's Edit Index.

| File | What |
|---|---|
| `EditIndexRows.h` | pure functions: `editindex::events()` (one row per clip, EDL-like) and `editindex::markers()` (timeline and clip markers in timeline order), `findMarker()` |
| `EditIndexModel.h` | `QAbstractTableModel` over those rows in two modes (Edits, Markers); marker Name / Notes editing as one undo step each |
| `EditIndexFilter.h` | `QSortFilterProxyModel`: search text over every column, marker colour filter, numeric sorting through `EditIndexModel::SortRole` |
| `EditIndexPanel.h` | the `PanelFrame`: mode box in the title bar, search field and colour menu, the table; jump / select on click, selection mirroring, coalesced live rebuilds |

## Behaviour
- **Edits:** #, Reel (file name without extension; the clip kind for generated clips), Name,
  Track, Source In / Out (source timecode, `ui/common/SourceTimecode.h`: the media's start
  timecode and rate), Record In / Out, Duration, Notes (transitions, speed, disabled,
  offline; read-only). Ordered by record in, then video, audio, subtitle tracks.
- **Markers:** #, Color, Name, Timecode, Duration (markers are single frames: 1 frame),
  Notes, Location ("Timeline" or the clip name). Clip markers trimmed out of their clip are
  not listed (they stay stored, spec 18.4).
- **Click / current row change** moves the playhead to the record in / marker and selects
  the clip (a clip marker's clip). A timeline selection change makes the clip's row current
  (Edits mode) without moving the playhead.
- **Live:** every `Timeline::changed()` / `MediaPool::changed()` schedules one rebuild per
  event loop pass; a hidden panel only marks itself dirty and rebuilds when it is shown.
- **Colours:** the table uses the palette (scheme); disabled clips use
  `SchemeColors::textDisabled`. Marker swatches show the marker's own colour (user data).

## How to extend
- **New column:** add it to the mode's column enum in `EditIndexModel.h` (before the
  `...ColumnCount` value), give it a header in `headerData()`, display / sort data in
  `eventData()` or `markerData()`, and the row field in `EditIndexRows.h` if it needs data
  the timeline does not give directly.
- **New editable marker field:** allow it in `EditIndexModel::flags()` and write it in
  `setData()` inside the one `beginEdit()` / `endEdit()` pair (named step).
- **New mode** (e.g. a clip list): add a `Mode` value, its column enum and row builder, and an
  entry in the panel's mode box.
