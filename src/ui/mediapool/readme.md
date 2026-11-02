# src/ui/mediapool — media pool, media browser, metadata

Owned by the panels agent.

| File | What |
|---|---|
| `MediaPoolPanel.h` | bin list + items (grid of poster frames / detail list), search, colour filter, import, rename, delete, reveal, move to bin, drag source; menus in `MediaPoolMenus.cpp`, undoable operations (bins, smart bins, power bins, colours, flags, usage commands) in `MediaPoolOrganize.cpp` |
| `BinList.h` | the bin list sidebar: Master tree, Smart Bins, Power Bins; drag to reorder / re-parent bins, drop items and files |
| `SmartBinDialog.h` | smart bin rules editor (name, match all / any, field / operator / value rows) |
| `MediaPoolModel.h` | `MediaPoolModel` (table over `MediaPool`, cached display strings, lazy posters) and `MediaPoolFilterModel` (bin + search) |
| `MediaPoolDelegate.h` | grid tile painter (poster or placeholder glyph, duration badge, offline marker, name) |
| `MediaBrowserPanel.h` | Media page file browser: places, folder tree, media-only file list, "Add to Media Pool" |
| `MetadataPanel.h` | Clip Details (read-only) + editable Shot and Scene / Notes fields; batch editing of several items (`attach()` follows a MediaPoolPanel) |

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

## Bins, smart bins, power bins (bins branch)
- Bin keys (MediaPoolFilterModel): "" = Master, bin id, "smart:<id>", "power:" (power root),
  "power:<id>". `currentBin()` stays a project bin id (new timelines go there); use
  `currentBinKey()` for the shown bin.
- Power bins come from `AppContext::powerBins` (null = no section). The model lists their
  items after the project's (`PowerRole`). Drags of power items carry `kPowerItemsMime`; the
  media ids are resolved (items copied into the project) only when a target reads
  `mime::MediaItems` (`MediaDragData::retrieveData`), so moves between power bins never touch
  the project.
- Usage counts are recomputed 150 ms after the last timeline / pool change; rows whose count
  changed emit dataChanged (smart bins with usage rules re-filter).
- List columns: `MediaPoolModel::Column`, keys `columnKey()`; visibility in
  `mediaPool/columns`; sorting through `SortRole` (raw numbers and times).
- `mediapool.*` organisation commands act only in the visible panel (both pages own one).

## Proxy / optimized media
`MediaPoolModel::setLinkStatusProvider()` (the app passes `MediaManagement::status()`) fills
the Proxy / Optimized Media columns and `LinkBadgeRole` (tile badge "P", "O", "P 42%",
amber "P!" when stale / missing); statuses are cached per row and refreshed on
`MediaManagement::statusChanged/statusReset`. The context menu shows the `mediapool.*`
commands (src/ui/common/MediaManagement); `publishSelection()` stores the selected ids in
their action data and enables them for the selection.

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
Import, rename, delete, bin create/rename/move/delete, smart bins, colours, flags, metadata and
moves are each one
`Project::beginEdit()/endEdit()` step (media pool state is part of the undo snapshot).

## How to extend
- **A new list column:** add it to `MediaPoolModel::Column`, fill a cached string in
  `makeRow()`, return it in `data()` and name it in `headerData()`.
- **A new filter (e.g. "video only"):** add state to `MediaPoolFilterModel` and test it in
  `filterAcceptsRow()` (call `invalidateFilter()` when it changes).
- **A new metadata field:** add it to `MediaMetadata` (core, with serialization in
  `metadataToJson()`), a `MetadataPanel::Field` with its editor, a list column, and (if
  wanted) a `SmartField` in core/SmartBins.
- **A new smart bin field:** add it to `SmartField`, `kFields` and `ruleMatches()` in
  core/SmartBins.cpp and to tests/core/test_smartbins.cpp.
- **Reveal and select the file** in the file manager: needs the FileManager1 D-Bus call; put
  it into `src/platform/` and call it from `MediaPoolPanel::revealInFileManager()`.
