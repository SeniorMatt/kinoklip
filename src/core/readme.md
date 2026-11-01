# src/core — data model, editing, undo, serialization

The heart of Kinoklip. No widgets, no FFmpeg calls (only the `media::MediaInfo` value type and
`media::probe()` for import). Everything here is unit-tested in `tests/core/`.

## Key types

| File | What |
|---|---|
| `Rational.h` | exact frame rates + frame/µs/sample conversions (PLAN §5.1) |
| `Animation.h` | `Interp`, `Keyframe`, `AnimatedValue` (keyframed scalar), `ParamSet` |
| `ParamKeys.h` | well-known parameter keys (`keys::PosX`, `keys::Opacity`, ...) + defaults |
| `TimelineData.h` | `ClipData`, `TrackData`, `Marker`, `EffectInstance`, `TransitionData`, `TimelineData`, `TrackRef` |
| `MediaItem.h` | `MediaItem`, `Bin`, `MediaPoolState` |
| `EditOps.h` | pure editing functions on `TimelineData&` (namespace `kk::editops`) |
| `Timeline.h` | QObject: owns `TimelineData`, snapshot cache, playhead, `changed()` |
| `MediaPool.h` | QObject: media items + bins, `importFiles()` |
| `Project.h` | QObject: owns MediaPool + Timeline + QUndoStack, settings, save/load |
| `SelectionModel.h` | QObject: selected clip ids + keyframes |
| `EffectRegistry.h` | effect/transition/generator/parameter-group descriptors |
| `BuiltinEffects.cpp` | the v0.1 descriptor catalogue |
| `Serialization.h` | JSON encode/decode of all value types |
| `Preferences.h` | QSettings preference keys + typed accessors (`playback/hwaccel`, autosave, theme) |

## Time conventions (PLAN §5.1)
- Timeline positions and durations: `int64_t` frames at `TimelineData::fps`.
- Source positions: `int64_t` microseconds (`ClipData::sourceInUs`).
- Keyframes: clip-local frames (`frame - clip.start`); they move with the clip and follow the
  content when the head is trimmed or the clip is split.
- `ClipData::sourceDurationUs` (addition to the PLAN contract) caches the media length so EditOps
  can clamp trims without a media-pool lookup; 0 means unbounded (stills, titles, solids).

## Performance notes (hot paths for render/playback)
- `Timeline::snapshot()` returns a cached `shared_ptr<const TimelineData>`; it deep-copies only
  once after each change. Render/playback threads keep the pointer as long as they need it.
- `AnimatedValue::valueAt()` is a binary search over sorted keys, allocation-free.
- `keys::*` are `QStringLiteral` QStrings, so `params.value(keys::Opacity, f)` never allocates.
- `TrackData::clipAt()` is a binary search; `EffectRegistry::find()` is a hash lookup.
- `Timeline::notifyChanged()` only emits a signal (no copies, no JSON), so live updates during a
  drag are cheap. The undo snapshot is taken once in `beginEdit()` and compared once in
  `endEdit()`.

## Editing protocol
```cpp
project->beginEdit(tr("Move Clip"));                    // snapshot "before"
editops::moveClips(project->timeline()->mutableData(), ids, +10);
project->timeline()->notifyChanged();                    // optional: live redraw during a drag
project->endEdit();                                      // pushes ONE undo command (if changed)
// or project->cancelEdit();                             // restores the "before" state
```
- Edits nest; only the outermost pair creates a command (its label wins).
- An undo command stores complete before/after copies of `TimelineData` and `MediaPoolState`.
  Its first `redo()` is a no-op because the change was already applied live.
- `Project::undo()/redo()` are ignored while an edit is open (e.g. mid-drag).
- Never mutate `mutableData()` outside begin/endEdit — the change would not be undoable and the
  project would not become dirty.

## Invariants (checked by `editops::checkInvariants`)
Clips on a track are sorted by start and never overlap; `duration >= 1`, `start >= 0`,
`speed > 0`, `sourceInUs >= 0`; every clip/track/effect id is non-zero, unique and `< nextId`;
locked tracks are never changed by EditOps. Ids come from `TimelineData::allocateId()`.

## Project file format (`.kkproj`, JSON)
```json
{ "formatVersion": 1, "generator": "Kinoklip 0.1.0",
  "project":   { "name": "My Film" },
  "mediaPool": { "bins": [...], "items": [{ "id", "path", "relativePath", "name", "binId", "info": {...} }] },
  "timeline":  { "name", "fps": {"num","den"}, "width", "height", "markIn", "markOut", "nextId",
                 "markers": [...], "videoTracks": [...], "audioTracks": [...] } }
```
Static params are plain JSON values except colours (`{"color": "#aarrggbb"}`).
Media paths are stored absolute and relative to the project file; the relative one is used when
the absolute path no longer exists.

## How to extend

**A new edit operation** (roll, slide, close gap, place on top, ...):
1. Declare a free function in `EditOps.h` with a `///` comment stating units and the return value.
2. Implement it in `EditOps.cpp` using the internal helpers (`moveHead`, `clearRangeImpl`,
   `insertSorted`, `GroupRemap`). Never touch locked tracks; keep tracks sorted.
3. Add tests in `tests/core/test_editops.cpp` ending with `KK_VERIFY_INVARIANTS(d)`, and add the
   operation to `randomOperationsKeepInvariants`.
4. Call it from the UI between `beginEdit()`/`endEdit()`.

**A new effect, transition or generator:**
1. Append a `descriptor(...)` to `EffectRegistry::registerBuiltins()` in `BuiltinEffects.cpp`
   (id = stable snake_case string stored in project files).
2. Implement the pixels in `src/render` keyed by the same id. The Inspector picks it up
   automatically from the descriptor.

**A new field in ClipData/TrackData/TimelineData:**
1. Add it with a default and a `///` comment (units!).
2. Write/read it in `Serialization.cpp` (missing on read → default, so old files keep loading).
3. Extend `richTimeline()` in `tests/core/test_serialization.cpp` so the round-trip covers it.
4. Bump `kFormatVersion` only for incompatible changes, and add a migration in `Project::load()`.
