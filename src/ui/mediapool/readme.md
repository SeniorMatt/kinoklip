# src/ui/mediapool — media pool, media browser, metadata

**Status: placeholders from the foundation agent; the panels agent owns this module.**

| File | What |
|---|---|
| `MediaPoolPanel.h` | icon list of `MediaPool` items, drop files to import; `currentMediaChanged`, `mediaActivated` |
| `MediaBrowserPanel.h` | `QFileSystemModel` tree of media files; double-click imports |
| `MetadataPanel.h` | read-only form of a `MediaItem`'s probe info |

## How to extend
Bins tree (`MediaPool::bins()`, `addBin/moveItemToBin`), thumbnails via `media::MediaCache`,
list/grid modes, drag to the timeline / source viewer. All imports go through
`importMediaWithFeedback()` (one undo step, warnings dialog).
