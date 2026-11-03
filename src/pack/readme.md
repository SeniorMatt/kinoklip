# `src/pack` — single-file projects

The `.kinoklip` container: one ZIP file (stored entries, ZIP64) with the project document,
a manifest and every media file, read in place. Format, crash safety and the reasons behind
them: [docs/single-file-project.md](../../docs/single-file-project.md).
Namespace `kk::pack`. Depends only on QtCore (the media engine uses `EmbeddedMedia.h`); no UI.

| File | What |
|---|---|
| `Container.h` | Low level: entries, append, `commit()` (fsync ordering, header slots), recovery at `open()`, `rollback()`. Not thread-safe; one writer per file. |
| `EmbeddedMedia.h` | Embedded paths (`film.kinoklip/media/<id>/clip.mp4`), the process-wide entry index cache (`locateEntry`), `RangeFile` (pread over a byte range, own handle) and `RangeDevice` (QIODevice). Thread-safe / one per reader. |
| `ProjectPackage.h` | A project in a container: runtime vs stored document form, manifest, `addMedia()` with duplicate detection, and the file operations Pack, Unpack, Compact, Verify, Repair (progress + cancel). |
| `Crc32.h` | CRC-32 (ZIP), slicing-by-8. |

The media engine reaches embedded media only through `media::MediaSource` (`src/media`).

## Users (sfint integration)
- `app/ProjectController` (`ProjectPackaging.cpp`): holds the open project's `ProjectPackage`
  and `projects/ProjectLock`, loads / saves the document, Save As between the kinds, File >
  Pack / Unpack / Compact / Verify, and copies imports in (`addMedia`, the AppContext's
  `MediaStore`).
- `projects/ProjectKinds` (the `.kinoklip` kind: metadata, rename, create) and
  `ProjectLibrary::exportProject()` (Pack / Unpack on export).
- The Media Pool's location indicator uses `splitEmbeddedPath()` (`ui/common/MediaImport.h`).
Behaviour: behavior-spec section 39.
