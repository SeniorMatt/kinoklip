# src/core — data model, editing, undo, serialization

The heart of Kinoklip. No widgets, no FFmpeg calls (only the `media::MediaInfo` value type and
`media::probe()` for import). Everything here is unit-tested in `tests/core/`.

## Key types

| File | What |
|---|---|
| `Rational.h` | exact frame rates + frame/µs/sample conversions (PLAN §5.1) |
| `Animation.h` | `Interp` (per-key preset), `KeyHandle`, `Keyframe`, `AnimatedValue` (keyframed scalar, Bezier evaluation), `ParamSet` |
| `Mask.h` | clip masks: `MaskShape`, `MaskMode` (combine table), `MaskPoint` / `MaskPath`, `AnimatedPath` (keyframed shape), `maskkeys::*`, `MaskData`; pure helpers (mask transform, outline, bezier flattening, polygon offset, freehand simplification, point insertion) |
| `KeyframeOps.h` | pure keyframe functions: parameter paths, grouped list of animated parameters, navigation, move / delete / interpolation / flatten / handles / copy and paste (namespace `kk::keyframeops`) |
| `ParamKeys.h` | well-known parameter keys (`keys::PosX`, `keys::Opacity`, ...) + defaults |
| `TimelineData.h` | `ClipData`, `TrackData`, `Marker`, `EffectInstance`, `TransitionData`, `TimelineData`, `TrackRef` |
| `MediaItem.h` | `MediaItem`, `Bin`, `MediaPoolState` |
| `EditOps.h` | pure editing functions on `TimelineData&` (namespace `kk::editops`); implemented in `EditOps.cpp`, `EditOpsRipple.cpp` (ripple delete across tracks, in/out lift / extract / copy, delete gaps, join, swap, shuffle, ripple overwrite) and `EditOpsTransitions.cpp` (transition windows, alignment, length, handle checks); shared private helpers in `EditOpsInternal.h` |
| `Timeline.h` | QObject: owns the active timeline's `TimelineData`, snapshot caches (render snapshot with nested timelines, plain snapshot), playhead, `changed()` |
| `MediaPool.h` | QObject: media items (files, timelines, compound clips) + bins, `importFiles()` |
| `Project.h` | QObject: owns MediaPool + Timeline + QUndoStack, every timeline of the project, tabs, settings, save/load; timelines / tabs / compound clips in `ProjectTimelines.cpp` |
| `CompoundOps.h` | pure compound-clip / nesting functions (namespace `kk::compound`): `collapse()`, `decompose()`, `canNest()` / `reaches()` (recursion), `syncNestedSources()`, `timelineInfo()`, `makeNestedClip()` |
| `Retime.h` | the speed model: constant speed or retime curve (`ClipData::retime`, keys = speed points), exact source-time mapping, segments, media limits, retime process (namespace `kk::retime`) |
| `Multicam.h` | multicam clips (namespace `kk::multicam`): `build()` from synced `AngleSource`s, `angleView()`, `gridTimeline()` (the viewer grid), `cutToAngle()` / `switchAngle()` / `flatten()`, `placeSynced()`, timecode parsing and sync offsets |
| `RetimeOps.h` | speed edits on a clip and its linked partners with ripple: speed points, segment speed, reverse / freeze segments, freeze frame, reset, retime / slide handles, ramps, Change Clip Speed (namespace `kk::retimeops`) |
| `SelectionModel.h` | QObject: selected clip ids + keyframes, or one transition (`TransitionRef`, exclusive) |
| `EffectRegistry.h` | effect/transition/generator/parameter-group descriptors |
| `BuiltinEffects.cpp` | the v0.1 descriptor catalogue |
| `Serialization.h` | JSON encode/decode of all value types |
| `Preferences.h` | QSettings preference keys + typed accessors (`playback/hwaccel`, autosave, theme) |
| `EditBehaviour.h` | convention-dependent editing defaults (`edit/*`: still / generator / transition lengths, three-point editing, ripple delete without selection, trim zones; `migrateSettings()` drops retired keys at start-up); see docs/behavior-spec.md |

## Time conventions (PLAN §5.1)
- Timeline positions and durations: `int64_t` frames at `TimelineData::fps`.
- Source positions: `int64_t` microseconds (`ClipData::sourceInUs`).
- Keyframes and clip markers (`ClipData::markers`): clip-local frames (`frame - clip.start`);
  they move with the clip and follow the content when the head is trimmed or the clip is split;
  a slip leaves keyframes in place. Keys a trim leaves outside the clip are kept (and return
  when the clip is extended).
- Transitions at a cut are placed by `TransitionData::alignment` (centre / start on cut / end
  on cut); `TransitionData::framesBeforeCut()` is the one formula render, painter and EditOps use.
- Retiming (`core/Retime.h`): the source time at clip-local frame x is
  `sourceInUs + (R(x) - R(0)) * frameUs` where R is the retime curve (source frames at the
  timeline rate) or `x * speed`. Speed points are keys of `keys::RetimeFrame` in
  `ClipData::retime` and shift with the content like keyframes; other keyframes stay at their
  clip time when a clip is retimed (spec 31.10).
- `ClipData::sourceDurationUs` (addition to the PLAN contract) caches the media length so EditOps
  can clamp trims without a media-pool lookup; 0 means unbounded (stills, titles, solids).

## Keyframe interpolation
Each `Keyframe` has an `in` and an `out` `KeyHandle` (mode Linear, Ease, Smooth or Free) and a
`hold` flag. The segment between keys A and B is the cubic Bezier through A, A + out(A),
B - in(B), B in (frame, value) space. Non-free handles are resolved from the neighbours and are
always a third of the segment long in time, which makes time linear in the Bezier parameter (no
root finding in the common case). Linear = points at the neighbour, Ease = flat, Smooth =
Catmull-Rom tangent flattened at extrema and the ends (no overshoot), Free = stored `dtFrames` /
`dv` (clamped to the segment, solved with bracketed Newton). Hold keeps the value until the
next key. `Interp` is the preset the user picks (Linear, EaseIn = arrives slowly, EaseOut =
leaves slowly, EaseInOut, Smooth, Hold); `keyInterp()` reports Custom for dragged handles.
`valueAtTime()` evaluates fractional frames (the audio gain envelope).

## Performance notes (hot paths for render/playback)
- `Timeline::snapshot()` returns a cached `shared_ptr<const TimelineData>`; it deep-copies only
  once after each change. Render/playback threads keep the pointer as long as they need it.
- `AnimatedValue::valueAt()` is a binary search over sorted keys, allocation-free.
- `keys::*` are `QStringLiteral` QStrings, so `params.value(keys::Opacity, f)` never allocates.
- `TrackData::clipAt()` is a binary search; `EffectRegistry::find()` is a hash lookup.
- `Timeline::notifyChanged()` only emits a signal (no copies, no JSON), so live updates during a
  drag are cheap. The undo snapshot is taken once in `beginEdit()` and compared once in
  `endEdit()`.

## Timelines, tabs, compound clips (v2)
- A project has any number of timelines, identified by `TimelineData::uid` (stable, never
  reused within a session). The **active** one lives in the single `Timeline` object
  (`Project::timeline()`), so every view connects once and follows tab switches through
  `Timeline::changed()`; `Project::activeTimelineAboutToChange` / `activeTimelineChanged`
  let views save and restore per-timeline state (`TimelineViewState`: zoom, scroll). Inactive
  timelines are immutable `shared_ptr<const TimelineData>` in the project; changing one
  replaces its pointer.
- Every timeline has a media pool item (`MediaItemKind::Timeline` / `CompoundClip`,
  `MediaItem::timelineUid`, no file). `Project::syncDerived()` runs at the end of every edit:
  item name -> timeline name, timeline -> item info (duration, size, rate, video/audio),
  missing items are recreated, orphan items removed, and `ClipData::sourceDurationUs` of every
  Nested clip follows the length of the timeline it shows.
- `ClipKind::Nested` + `ClipData::nestedUid`: a compound clip or nested timeline. Its source
  time is the nested timeline's time (µs), so EditOps treat it like media. `nestedFrameAt()`
  maps a parent frame to a nested frame. `compound::canNest()` refuses anything that would make
  a timeline contain itself (directly or indirectly); renderers also stop at depth 16.
- `Timeline::snapshot()` (render threads) fills `TimelineData::nested` with every timeline the
  root reaches (shared pointers, no copies); `plainSnapshot()` is the same without the map
  (undo history). `NestedTimelines` compares equal to everything, so it never makes undo
  snapshots differ. `Project::renderSnapshot(uid)` does the same for any timeline.
- Tabs (`Project::tabs()`, `openTab()`, `closeTab()`, `moveTab()`) are UI state saved with the
  project, not undoable. `setActiveTimeline()` is refused while an edit is open.

## Multicam clips (v2)
- A multicam clip is a compound timeline with `TimelineData::multicam` set: video track k +
  audio track k = angle k (track names = angle names). Its media pool item has
  `MediaItemKind::MulticamClip`; `Project::createMulticamClip()` / `flattenMulticamClip()`
  (ProjectMulticam.cpp) are one undo step each.
- A Nested clip showing it carries `ClipData::multicamAngle` (-1 = whole timeline, as every
  other Nested clip). `Project::nestedTimelinesFor()` adds one *angle view* per angle to the
  render snapshot's map under `multicamViewKey(uid, angle)` (only that angle's tracks; cached
  until the multicam timeline changes), and every renderer looks clips up with
  `NestedTimelines::findClip()`, so only the active angle is decoded, drawn and mixed.
- Cuts split both sides of a segment (V/A stay pairwise linked) and switch the target side.
- Sync offsets (`AngleSource::offsetUs`): where media time 0 of each source lies on a common
  axis; `build()` starts the earliest at frame 0 and keeps sub-frame alignment through
  `sourceInUs`. Waveform offsets come from `media/AudioSync.h`.

## Editing protocol
```cpp
project->beginEdit(tr("Move Clip"));                    // snapshot "before"
editops::moveClips(project->timeline()->mutableData(), ids, +10);
project->timeline()->notifyChanged();                    // optional: live redraw during a drag
project->endEdit();                                      // pushes ONE undo command (if changed)
// or project->cancelEdit();                             // restores the "before" state
```
- Edits nest; only the outermost pair creates a command (its label wins).
- An undo command stores before/after copies of every timeline (`Project::TimelineSet`: shared
  pointers, so unchanged timelines cost nothing and compare by pointer), the `MediaPoolState`
  and the project format. Its first `redo()` is a no-op because the change was already applied
  live. Undoing a step made in another timeline first activates that timeline.
- Assigning a whole `TimelineData` to `mutableData()` is fine: the active timeline keeps its
  uid (endEdit restores it).
- `Project::undo()/redo()` are ignored while an edit is open (e.g. mid-drag).
- Never mutate `mutableData()` outside begin/endEdit — the change would not be undoable and the
  project would not become dirty.

## Auto track selectors
`TrackData::autoSelect` (default on) marks the tracks that take part in edits without an
explicit target: `editops::autoSelectTracks()` is the filter for next/previous edit point,
split at the playhead without a selection, mark clip and the default transition, and
`RippleScope::AllTracks` (insert, close gap) skips tracks whose selector is off unless they
receive clips.

## Masks (`Mask.h`, roadmap v2)
`ClipData::masks` is an ordered list; the renderer applies it to the clip's picture after its
effects and before its transform (src/render/readme.md, "Masks"). Mask coordinates are clip
content pixels (timeline pixels at zoom 1, origin at the content centre); point paths are
mask-local and mapped by `maskToContent()` (position + rotation + scale). Numeric parameters
live in `MaskData::params` (`maskkeys::*`, KeyframeOps path `mask/<id>/<key>`); mode, invert,
enabled and closed are struct fields. The shape of point masks is ONE animated property
(`AnimatedPath`, path `mask/<id>/shape`): an ordinary `AnimatedValue` holds the key frames and
easing and its key values index the stored shapes, so every generic keyframe operation works on
shape keys; KeyframeOps calls `AnimatedPath::normalize()` after moving / deleting them (and
skips them in vertical curve drags). All shapes of a path have the same point count
(`insertPathPoint()` / `removePathPoint()` edit every key). Mask ids share the timeline id space;
split and paste give copies fresh ids.

## Invariants (checked by `editops::checkInvariants`)
Clips on a track are sorted by start and never overlap; `duration >= 1`, `start >= 0`,
`speed > 0`, `sourceInUs >= 0`; every clip/track/effect/mask id is non-zero, unique and `< nextId`;
locked tracks are never changed by EditOps. Ids come from `TimelineData::allocateId()`.

## Project file format (`.kkproj`, JSON)
```json
{ "formatVersion": 4, "generator": "Kinoklip 0.1.0",
  "project":   { "name": "My Film", "width", "height", "fps": {"num","den"} },
  "mediaPool": { "bins": [...], "items": [{ "id", "path", "relativePath", "name", "binId", "info": {...} },
                                          { "id", "kind": "timeline" | "compound", "timelineUid", "name", "binId", "info" }] },
  "timelines": [{ "uid", "compound"?, "useProjectSettings"?, "name", "fps", "width", "height",
                  "markIn", "markOut", "nextId", "markers": [...], "videoTracks": [...], "audioTracks": [...] }],
  "activeTimeline": uid, "tabs": [uid, ...],
  "timelineViews": [{ "uid", "playhead", "pixelsPerFrame", "scrollX", "scrollY" }] }
```
Nested clips: `"kind": "nested", "nestedUid"`. Retimed clips (version 5):
`"retime": {"speed", "curve"?: ParamSet with "retimeFrame", "process"?, "pitchCorrection"}`;
timelines and `"project"` carry `"retimeProcess"`. A version <= 4 clip's `"speed"` loads as one
constant segment with pitch correction off. Versions 1 and 2 had a single `"timeline"`
object instead of `"timelines"`; `Project::fromJson()` loads it as timeline uid 1 and creates
its media pool item, so old projects open unchanged.
Static params are plain JSON values except colours (`{"color": "#aarrggbb"}`).
Keyframes: `{"frame", "value"}` plus, when not default, `"in"` / `"out"` (handle mode string,
or `{"mode": "free", "dt", "dv"}`) and `"hold": true`. Format version 1 stored one `"interp"`
per key (curve of the segment starting there); `paramSetFromJson()` recognises it and converts
it with `migrateLegacyInterp()` (ease in and out is reproduced exactly).
Format version 4 adds clip `"masks"` (see `Serialization.h` for the fields); files of versions
1 - 3 have none and load unchanged.
Format version 8 adds multicam: timeline `"multicam": true`, clip `"multicamAngle"`, media
pool item `"kind": "multicam"`, media info `"timecode"` (all optional; 6 and 7 are reserved for
the proxies / adjustment-clip branches).
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
