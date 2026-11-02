# src/ui/mediapool — media pool, media browser, metadata

Owned by the panels agent.

| File | What |
|---|---|
| `MediaPoolPanel.h` | bins tree + items (grid of poster frames / detail list), search, import, rename, delete, reveal, move to bin, drag source |
| `MediaPoolModel.h` | `MediaPoolModel` (table over `MediaPool`, cached display strings, lazy posters) and `MediaPoolFilterModel` (bin + search) |
| `MediaPoolDelegate.h` | grid tile painter (poster or placeholder glyph, duration badge, offline marker, name) |
| `MediaBrowserPanel.h` | Media page file browser: places, folder tree, media-only file list, "Add to Media Pool" |
| `MetadataPanel.h` | probe info of the current item (Clip / Video / Audio sections, offline state) |

## Contracts for other modules
- `MediaPoolPanel::openInSourceViewer(QString mediaId)` — emitted on double-click / context
  menu. The Edit page connects it to `ViewerPanel::setSourceMedia` (src/app/pages/EditPage.cpp).
- `MediaPoolPanel::currentMediaChanged(QString mediaId)` — selection, for the metadata panel and
  the Media page viewer.
- `MediaPoolPanel::selectMedia(id)` — e.g. "find clip in media pool" from the timeline.
- Drags carry `mime::MediaItems` (newline-separated ids, `ui/common/DragMime.h`).
- Poster frames: `PosterProvider` = `std::function<QImage(path, height)>`, non-blocking
  (null = pending). Default: `media::MediaCache::instance().posterFrame()`; the panel connects
  `MediaCache::posterFrameReady` to `MediaPoolModel::thumbnailReady()`. Replace it with
  `MediaPoolPanel::setThumbnailProvider()` (tests use a synchronous fake).

## Multicam and sync (MediaPoolMulticam.cpp, MulticamDialog, MulticamSync)

Context menu for video / audio items: Create New Multicam Clip Using Selected Clips… (dialog:
name, angle sync, angle names, Original Clips bin) and Sync and Place on Timeline (Based on
Waveform / Timecode / In Points). `computeSync()` (no widgets, testable) does the work;
`runSyncWithFeedback()` runs waveform analysis on a worker behind a cancellable progress
dialog and reports clips that could not be synced. In points come from
`setInPointProvider()` (EditPage: the source viewer's remembered marks). Multicam items
double-click into the source viewer (their angle grid) instead of a timeline tab.

## Performance (2,000+ items)
- One shared model + proxy + selection for both views; `QListView` with uniform item sizes,
  fixed grid size and batched layout.
- Display strings formatted once per item change; `data()` only returns cached values.
- Offline state: one `QFileInfo::exists()` per item on add/reset and when the application is
  re-activated; never during painting.
- Posters are requested only for painted tiles, once per path while pending, pre-scaled to
  `kPosterSize` and kept in a 48 MB `QCache` (evicted tiles are re-fetched from MediaCache's
  own memory/disk cache).
- Measured (offscreen, test_mediapoolpanel): 10 full repaints of a 2,000-item grid in ~40 ms;
  search filter over 2,000 items < 1 ms.

## Layout notes
The bins sidebar follows the panel width (hidden below 460 px so the narrow Edit-page column
keeps two tile columns) until the user toggles it with the folder button; that choice is then
remembered (`mediaPool/binsVisible`). The grid/list choice is remembered as `mediaPool/viewMode`.

## Undo
Import, rename, delete, bin create/rename/delete and moves are each one
`Project::beginEdit()/endEdit()` step (media pool state is part of the undo snapshot).

## How to extend
- **A new list column:** add it to `MediaPoolModel::Column`, fill a cached string in
  `makeRow()`, return it in `data()` and name it in `headerData()`.
- **A new filter (e.g. "video only"):** add state to `MediaPoolFilterModel` and test it in
  `filterAcceptsRow()` (call `invalidateFilter()` when it changes).
- **Editable metadata (scene, take, notes):** add fields to `MediaItem` (core) with
  serialization, then editors in `MetadataPanel::showMedia()` writing through
  `beginEdit()/endEdit()`.
- **Reveal and select the file** in the file manager: needs the FileManager1 D-Bus call; put
  it into `src/platform/` and call it from `MediaPoolPanel::revealInFileManager()`.
