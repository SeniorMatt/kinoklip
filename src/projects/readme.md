# src/projects — the project library

The non-UI side of the Project Manager (spec section 38): which projects exist, what they
contain (without opening them in the editor), and the file operations on them. Depends on
`core` only (serialization, `Project` for creating `.kkproj` files); used by `ui/projects` and
`app/ProjectController`. Nothing here includes widgets.

## Key classes

| File | What |
|---|---|
| `ProjectKinds.h` | `ProjectMetadata` (name, resolution, fps, duration, timelines, media / missing media, poster source, error); `ProjectStorage` (Linked / SingleFile); `ProjectKind` (id, badge, extensions, metadata reader, name writer, creation factory) and its registry (`projectkinds::all()`, `registerKind()`, `forPath()`, `byId()`, `byStorage()`); the `.kkproj` reader / writer / factory |
| `ProjectLock.h` | one writer per project file: a `QLockFile` per path in `<AppDataLocation>/locks` (never next to the project); also refuses a second holder in the same process (a second window); a crashed holder's lock is taken over |
| `ProjectLibrary.h` | the library folder: `list()` (one readdir, no file opened), lazy `metadata()` on a 2-thread pool with the sidecar cache, posters, the open project's protection, create / rename / duplicate / move / trash / import / export |

## Data flow
1. A view lists a folder: `ProjectLibrary::list()` -> `ProjectEntry` (path, name, mtime, size,
   kind). Folders of the library are folders on disk; projects are files of a registered kind.
2. The view paints a card: `metadata(entry)` returns the cached value if path + mtime + size
   match, else schedules the kind's `readMetadata` on the pool -> `metadataReady(path)`.
3. Posters: `poster(entry)` loads `<cache>/posters/<sha1(path)>-<mtime>.jpg` on the pool
   (`posterReady`); if absent, the UI model asks `media::MediaCache` for the thumbnail of
   `ProjectMetadata::posterMediaPath` at `posterTimeUs` (the frame under the active timeline's
   saved playhead, else its first video clip) and hands it back with `storePoster()`.
4. The metadata cache `<cache>/metadata.json` (`<QStandardPaths::CacheLocation>/project-library` by default, under `$XDG_CACHE_HOME`)
   is written one second after the last change and on destruction. Entries of deleted files are
   dropped when it is written. Nothing is stored in the project files (no format change).

## Rules
- The open project (`setOpenProjectPath()`) and folders containing it cannot be deleted,
  renamed or moved (its path is the editor's and the autosave's document path).
- Delete = `QFile::moveToTrash()` (freedesktop trash: `$XDG_DATA_HOME/Trash` on the home
  partition, `.Trash-<uid>` at the top of other partitions).
- Rename changes the file name and the name stored inside the project (`ProjectKind::writeName`).
- Import / duplicate / move copy or rename the file unchanged: media paths are stored absolute
  (the relative path is only the fallback), so the copy finds the same media; a single-file
  project carries its media along.

## How to extend

**The single-file project (`.kinoklip`, src/pack):** the built-in kind `kinoklip` reads the
document inside the container read-only (`readKinoklipMetadata()`, never the media), renames
by saving the document anew under the project lock (`writeKinoklipName()`) and creates an
empty container with a document (`createKinoklip()`). Export to the other kind packs or
unpacks (`ProjectLibrary::exportProject()`, may run on a worker thread).

**Another kind of project file:** register a `ProjectKind` with a new id and extension; the
library lists it, the cards show its badge, Import accepts it.

**Another metadata field:** add it to `ProjectMetadata`, read it in the kind's reader, write
and read it in `metaToJson()` / `metaFromJson()` (`ProjectLibrary.cpp`) and bump
`kCacheVersion` there so old caches are rebuilt.
