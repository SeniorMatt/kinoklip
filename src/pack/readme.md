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

## Not here (integration step)
Project Manager, File menu (Pack Project, Unpack, Compact), import into a self-contained
project and the Media Pool location indicator call these functions; see the plan in
docs/single-file-project.md and behavior-spec section 39.
