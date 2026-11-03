# Single-file projects (`.kinoklip`)

A single-file project is one file that holds the whole project: the project document (the same
JSON as a linked `.kkproj`), every media file it uses, unchanged, and a small manifest. It can be
moved, copied or mailed like any other file, and media play straight from inside it.

Code: `src/pack/` (container library, no UI), the media engine's `MediaSource`
(`src/media/MediaSource.h`). Tests: `tests/pack/test_container.cpp`,
`tests/pack/test_projectpackage.cpp`, `tests/media/test_embeddedmedia.cpp`,
`tests/export/test_embeddedexport.cpp`. Behaviour: `docs/behavior-spec.md` section 39.

## Why ZIP

The file is a standard ZIP archive (PKWARE APPNOTE 6.3) with **stored** (uncompressed) entries
and **ZIP64** records. Every unzip tool (Info-ZIP `unzip`, 7-Zip, Windows Explorer, Python's
`zipfile`, macOS Archive Utility) lists and extracts it, so media can always be recovered without
Kinoklip. Stored entries are the original bytes at a fixed offset, which is what reading in place
needs. Compression is never used: video, audio and image formats are already compressed, and a
compressed entry cannot be read at random offsets.

Alternatives considered: SQLite (a database with media as blobs: no in-place byte ranges for
FFmpeg, unreadable without SQLite tools, page-level rewrites), a custom format (no recovery tools),
a folder bundle like macOS packages (not one file on Linux / Windows).

## Layout

```
offset 0  local header "mimetype"  + private extra field 0x4b4b with two header slots (64 bytes)
          "application/x-kinoklip"                (first entry, stored, like ODF / EPUB)
          local header "media/3fa9c2d1/clip.mp4" + the file's bytes
          local header "media/88e01b5a/still.jpg" + ...
          local header "manifest.json"  + JSON
          local header "project.kkproj" + JSON    (the project document)
          central directory (live entries only)
          ZIP64 end of central directory record, ZIP64 locator
          end of central directory record, comment:
          "Kinoklip project container 1; gen <16 hex digits>; crc <8 hex digits>"
-- a save appends: --
          local header "project.kkproj" + JSON    (new version)
          local header "manifest.json"  + JSON
          new central directory + ZIP64 records + end record (gen + 1)
```

* All entries are stored (method 0), names are UTF-8 (flag bit 11), external attributes are Unix
  `0644`. Entry names are relative, `/`-separated, never contain `..`, `.` or `\`.
* An entry of 4 GiB or more, or one starting beyond 4 GiB, gets the ZIP64 extra field (id 1) in
  the local header (sizes) and the central directory (sizes and / or offset, in APPNOTE order).
  ZIP64 end records are always written, so the format does not change at the 4 GiB boundary.
* Media entries are `media/<id>/<original file name>`: `<id>` is 8 random hex digits unique in the
  file. The name never changes while the entry exists; bins, clip names and moves between bins
  live in the project document only, so organising the Media Pool never touches media bytes.
* Old versions of the document stay in the file as unreferenced bytes until **Compact**.
  Standard ZIP readers use the last central directory and ignore them.
* Caches (thumbnails, waveforms, proxies, optimised media, render cache) are never stored; they
  are rebuilt next to the user's other caches. The disk cache identity of an embedded file is
  container path + entry + CRC + size (stable across saves and Compact).

### The comment

The end record's comment has a fixed length (64 bytes) and carries the commit **generation**
and the **CRC-32 of the central directory plus the ZIP64 records**. A reader accepts an end
record only if the comment parses, the ZIP64 records sit exactly in front of it, the central
directory ends exactly at the ZIP64 record, the CRC matches, every central entry has a matching
local header (same name) with its data before the directory, and every small non-media entry
(below 64 MiB, outside `media/`: the document, the manifest, the mimetype) has the right CRC.

### Header slots

The first entry's local header has a private extra field (id `0x4b4b`, 64 bytes) with two
32-byte slots: `"KKS1"`, generation (u64), offset of the end record (u64), committed file size
(u64), CRC-32 of the first 28 bytes. A commit writes the slot `generation % 2`, so the other slot
always describes the previous commit. ZIP readers ignore unknown extra fields, and the extra field
is not covered by any CRC, so the slots can be rewritten in place. (The mimetype entry's central
directory record has no extra field; ZIP tools do not compare the two.)

## Commit protocol (saving)

1. If the file is longer than the committed state (a torn earlier save), cut it back.
2. Append the new entries (document, manifest; media when importing). `fdatasync`.
3. Append the central directory, ZIP64 records and end record. `fdatasync`.
4. Write the header slot of the new generation. `fdatasync`.

Saving the project therefore writes the document and the manifest, a few KiB to a few MiB, never
media. Importing writes the media once.

## Recovery (opening)

1. **Normal case:** the end record at `size - 86` is valid: use it.
2. **Torn save:** take the valid header slots, newest generation first, and use the first whose
   end record is valid (same generation in the comment). A crash during step 2 or 3 leaves the
   new slot unwritten, so the previous one is found; a crash during step 4 leaves a torn slot
   (its CRC fails) but a valid end record at the end of the file (case 1).
3. **Last resort** (both slots destroyed): scan backwards for an end record signature and use the
   first valid one.

Bytes after the committed state are ignored when reading and cut off by the next write
(`repairContainer()` does this explicitly so standard tools read the file again). The truncation
test cuts the file at every byte near both ends of the last save and every 7 bytes in between;
each cut opens with the previous document.

One writer per file at a time; the application holds the project lock (as for `.kkproj`).
Readers in other threads or processes see only committed states: committed entries never move.

## Media references in the project document

The document format (`formatVersion` 13) does not change. A media pool item stored in a
single-file project has one extra field:

```json
{ "id": "...", "embedded": "media/3fa9c2d1/clip.mp4",
  "path": "media/3fa9c2d1/clip.mp4", "relativePath": "media/3fa9c2d1/clip.mp4", ... }
```

* `embedded` marks the item as stored in the container. `path` and `relativePath` hold the same
  relative name, so the document contains no absolute paths, and an archive extracted with any
  unzip tool opens as an ordinary linked project (the loader's relative-path fall-back finds
  `media/...` next to `project.kkproj`), even in builds older than this feature (they ignore the
  unknown field).
* Linked media in a single-file project (offline at Pack time, image sequences) keep their normal
  absolute `path` / `relativePath` and no `embedded` field.
* **Runtime form.** `documentToRuntime()` turns every embedded item's path into an *embedded path*
  `<absolute container path>/<entry>`, e.g. `/home/me/film.kinoklip/media/3fa9c2d1/clip.mp4`;
  `documentToStorage()` turns them back when saving. The rest of the application sees ordinary
  path strings: the suffix and file name are those of the media file, so format routing by
  extension works. A path is embedded when a component ends in `.kinoklip` and that prefix is a
  regular file (`pack::splitEmbeddedPath`).

Since no field changes meaning, no format version bump and no migration is needed. The only
loader change: `mediaPoolFromJson()` checks existence with `media::mediaExists()`, so an embedded
path counts as present (otherwise the "same file name next to the project" fall-back could pick
an unrelated file).

## The manifest

```json
{ "format": "kinoklip-project", "version": 1, "generator": "Kinoklip 0.3.6",
  "document": "project.kkproj",
  "media": [ { "entry": "media/3fa9c2d1/clip.mp4", "originalPath": "/home/me/clip.mp4",
               "size": 123, "modifiedMs": 1730000000000, "crc32": 305419896,
               "addedMs": 1730000000000 } ] }
```

`originalPath`, `size` and `modifiedMs` recognise a re-import of the same file; Unpack restores
the modification time. A reader refuses a manifest `version` newer than it knows.
TODO(pack): an option to drop `originalPath` when sharing a project (privacy).

## Reading in place

`media::MediaSource` is the single abstraction for "where are a media path's bytes" and is used
everywhere the media engine opens a file:

| Consumer | How embedded media are read |
|---|---|
| FFmpeg (probe, VideoDecoder incl. hardware decoding, AudioDecoder, FFmpeg stills, export / render, thumbnails, waveforms) | `openInput()` creates a custom `AVIOContext` (256 KiB buffer) with read and seek callbacks over the entry's byte range; the entry's file name is passed as the URL so demuxers that score by extension still work |
| Qt image readers (stills, animated WebP) | `QImageReader` on a `pack::RangeDevice` (random-access `QIODevice` over the range), format from the extension |
| libheif, LibRaw | the entry read into memory (`heif_context_read_from_memory_without_copy`, `LibRaw::open_buffer`) |
| caches | `MediaSource::identity()` (still cache, disk cache directories) |

Every reader has its **own file handle** and reads with `pread()` (POSIX), so any number of
decoders on any threads read one container at once without a shared seek position. Entry
locations come from a process-wide index cache (`pack::locateEntry`), re-read when the
container's size or modification time changes.

## Operations (library: `src/pack/ProjectPackage.h`)

All long operations take a `Progress` callback (bytes done, total; return false to cancel) and
copy in 8 MiB chunks. Cancelling, failing or crashing never leaves the file invalid.

| Operation | Function | Notes |
|---|---|---|
| Save | `ProjectPackage::saveDocument()` | appends document + manifest, commits |
| Add media (import) | `ProjectPackage::addMedia()` | copies files in and commits once; the same file is stored once (same original path, size and time; or same size, CRC and bytes); runs of zeros stay holes (sparse) |
| Pack | `packProject(kkproj, out.kinoklip)` | copies every used media file; writes `out.kinoklip.part`, renames when complete; offline media and image sequences stay linked and are reported |
| Unpack | `unpackProject(container, folder)` | media to `folder/media/<id>/<name>` (CRC checked, times restored) and `folder/<name>.kkproj`; never overwrites |
| Compact | `compactProject(container)` | rewrites without unreferenced media and old documents into `.<name>.compact-<pid>.part` in the same folder, fsync, atomic rename; CRC-checks everything it copies; reports size before / after; `compactedSizeEstimate()` previews |
| Verify | `verifyContainer(container, checkData)` | index, overlaps, document, manifest, missing media, CRC of every byte |
| Repair | `repairContainer(container)` | cuts a torn tail and writes a fresh end record |

Compact keeps media referenced by any media pool item. Media in the pool but on no timeline is
kept; the UI can first offer "Remove Unused Clips" from the Media Pool.
TODO(pack): Pack with trimming (only the used ranges of long clips plus handles) needs
re-muxing per container format and keyframe-aware cut points; not done.
TODO(pack): image sequences inside the container (one entry per frame, sequence scanning over
entry names).
TODO(pack): Compact on Windows must close every open reader first (rename over an open file
fails there); on POSIX readers keep reading the old inode until they reopen.

## Size limits

ZIP64 throughout: files and entries up to 2^63 bytes. The central directory is limited to
256 MiB (about a million entries). Tested: a 4 GiB + 120 KiB sparse entry followed by an entry
starting beyond 4 GiB, read back by Kinoklip and by Python's `zipfile`.
