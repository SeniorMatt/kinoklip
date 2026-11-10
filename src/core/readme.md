# src/core — data model, editing, undo, serialization

The heart of Kinoklip. No widgets, no FFmpeg calls (only the `media::MediaInfo` value type and
`media::probe()` for import). Everything here is unit-tested in `tests/core/`.

## Key types

| File | What |
|---|---|
| `SessionLog.h` | the session log for Help > Export Log File (spec 64): `RingBuffer` (last 20,000 lines / 4 MiB, oldest dropped and counted), `installMessageHandler()` (called first in `main()`; keeps warnings of every category and info of `kk.*` / `kinoklip.*`, then passes the message on to the previous handler, so stderr is unchanged), `record()` for lines that are not printed (the per-second playback statistics) |
| `Rational.h` | exact frame rates + frame/µs/sample conversions (PLAN §5.1) |
| `Animation.h` | `Interp` (per-key preset), `KeyHandle`, `Keyframe`, `AnimatedValue` (keyframed scalar, Bezier evaluation), `ParamSet` |
| `Mask.h` | clip masks: `MaskShape`, `MaskMode` (combine table), `MaskPoint` / `MaskPath`, `AnimatedPath` (keyframed shape), `maskkeys::*`, `MaskData`; pure helpers (mask transform, outline, bezier flattening, polygon offset, freehand simplification, point insertion) |
| `KeyframeOps.h` | pure keyframe functions: parameter paths, grouped list of animated parameters, navigation, move / delete / interpolation / flatten / handles / copy and paste (namespace `kk::keyframeops`) |
| `ParamKeys.h` | well-known parameter keys (`keys::PosX`, `keys::Opacity`, ...) + defaults |
| `TimelineData.h` | `ClipData`, `TrackData`, `Marker` (frame, name, colour, note, duration: ranged markers, format 13), `EffectInstance`, `TransitionData`, `TimelineData`, `TrackRef` |
| `UndoSnapshots.h` | compact undo storage (`undosnap::pack()` / `unpack()`): a timeline as a clip-less shell plus shared immutable clips, packed against the neighbouring step so a step owns only the clips it changed |
| `MediaItem.h` | `MediaItem`, `Bin`, `MediaPoolState` |
| `EditOps.h` | pure editing functions on `TimelineData&` (namespace `kk::editops`); implemented in `EditOps.cpp`, `EditOpsRipple.cpp` (ripple delete across tracks, in/out lift / extract / copy, delete gaps, join, swap, shuffle, ripple overwrite) and `EditOpsTransitions.cpp` (transition windows, alignment, length, handle checks); shared private helpers in `EditOpsInternal.h` |
| `Timeline.h` | QObject: owns the active timeline's `TimelineData`, snapshot caches (render snapshot with nested timelines, plain snapshot), playhead, `changed()` |
| `MediaPool.h` | QObject: media items (files, timelines, compound clips) + bins (nested, ordered, `moveBin()`, `removeBinWithContents()`), clip colours, flags, metadata, smart bins, `importFiles()` |
| `ClipColors.h` | the sixteen clip colours, sixteen flag colours and sixteen marker colours (stable ids, our own names), lookups, `nearestMarkerColor()` |
| `SmartBins.h` | smart bin rules: field / operator catalogue, `ruleMatches()` / `matches()`, default smart bins |
| `MediaUsage.h` | usage counts of media pool items over every timeline (`usage::compute()`), clips using items, unused files |
| `PowerBins.h` | QObject: application-wide MediaPool saved to `<AppData>/power-bins.json` on every change |
| `Project.h` | QObject: owns MediaPool + Timeline + QUndoStack, every timeline of the project, tabs, settings, save/load; timelines / tabs / compound clips in `ProjectTimelines.cpp` |
| `CompoundOps.h` | pure compound-clip / nesting functions (namespace `kk::compound`): `collapse()`, `decompose()`, `canNest()` / `reaches()` (recursion), `syncNestedSources()`, `timelineInfo()`, `makeNestedClip()` |
| `Retime.h` | the speed model: constant speed or retime curve (`ClipData::retime`, keys = speed points), exact source-time mapping, segments, media limits, retime process (namespace `kk::retime`) |
| `Multicam.h` | multicam clips (namespace `kk::multicam`): `build()` from synced `AngleSource`s, `angleView()`, `gridTimeline()` (the viewer grid), `cutToAngle()` / `switchAngle()` / `flatten()`, `placeSynced()`, timecode parsing and sync offsets |
| `RetimeOps.h` | speed edits on a clip and its linked partners with ripple: speed points, segment speed, reverse / freeze segments, freeze frame, reset, retime / slide handles, ramps, Change Clip Speed (namespace `kk::retimeops`) |
| `SelectionModel.h` | QObject: selected clip ids + keyframes, or one transition (`TransitionRef`, exclusive); the active mask and the viewer eyedropper's target (`ColorPickTarget`) |
| `EffectRegistry.h` | effect/transition/generator/parameter-group descriptors |
| `AudioTransitions.h` | audio transition ids (Effects library "Audio Transitions") and their pure gain curves `gainIn()` / `gainOut()` (0 dB linear, -3 dB, +3 dB equal power, exponential, logarithmic, S-curve); any other id is +3 dB |
| `Composition.h` | Compositing page node graph of a clip: `Composition`, `CompNode`, `NodeLink`, `CompNote` (`ClipData::composition`) |
| `NodeRegistry.h` | node type descriptors (ports, parameters, viewer controls), ids / port ids / parameter keys, the `fx:<id>` node of every video effect |
| `CompOps.h` | pure graph operations: default composition, add / add after (auto-merge) / remove / connect / disconnect / insert into link / paste, cycle and kind checks, evaluation order |
| `Grade.h` | Color page grade of a clip or timeline: `ClipGrade`, `GradeNode` (corrector / Parallel / Layer), curves, qualifier, power windows, LUT; parameter keys (`gradekeys`, `gradewindowkeys`); pure graph operations (`gradeops`). `Project::setClipGrade()` / `setTimelineGrade()` (ProjectGrades.cpp) are the undoable entry points (docs/color-page.md) |
| `GradeMath.h` | the grading maths shared by the CPU evaluator and mirrored in GLSL: primaries, curves (monotone cubic, HSL curves), qualifier key, window coverage, layer formula |
| `GradeGallery.h` | gallery stills (picture + grade), saved with the project, `Project::gallery()` |
| `Stabilization.h` | Stabilization of a clip (spec 58): `Settings`, the measured camera path `Data` (one entry per source frame; Smooth / Camera Lock / Strength / Zoom worked out from it lazily), `ClipStabilization` (`ClipData::stabilization`), `transform()` for the compositor, JSON form |
| `Generators.h` | ids and parameter keys of the built-in generators and the adjustment clip |
| `TitlePresets.h` | ids of the Effects library titles (Text, Left / Middle / Right Lower Third, Scroll, Text Box): one Title parameter set with each preset's defaults; `titles::descriptorOf()` finds the preset a Title clip shows (`ClipData::generator`) |
| `Subtitles.h` | `SubtitleCue`, SRT / WebVTT parse and write, caption style keys + defaults (caption → track → default chain), cue ↔ caption conversion (lanes for overlapping cues) |
| `ViewerGuides.h` | `ViewerGuide` (orientation + position in timeline frame px; `TimelineData::guides`), its JSON, guide presets (built-in, user file `<AppData>/guide-presets.json`, in fractions of the frame), safe area / aspect marker / grid geometry, `snapOffset()` and ruler steps (spec 41) |
| `AudioEffectKeys.h`, `BuiltinAudioEffects.cpp` | ids, parameter keys and descriptors of the clip audio effects (category `AudioEffect`, stored in the audio clip's `ClipData::effects`; processors in `render/AudioClipEffects.*`) |
| `BuiltinEffects.cpp` | the v0.1 descriptor catalogue |
| `CutEdits.h` | the Cut page's smart edits as pure functions (namespace `kk::cutedits`): `nearestEditPoint()`, `smartInsert()`, `append()`, `rippleOverwrite()`, `placeOnTop()` (one free track for the whole edit), `closeUp()` (zoomed video on top), `sourceOverwrite()` + `sourceOverwriteStart()` (line up by timecode), `fastReviewRate()`; spec section 35 |
| `SourceTape.h` | the Cut page's source tape: a bin's clips back to back at the timeline rate; tape frame <-> (clip, source µs), a marked range split at the clip boundaries (`segments()`), a playable private timeline (`toTimeline()`) |
| `Serialization.h` | JSON encode/decode of all value types |
| `Preferences.h` | QSettings preference keys + typed accessors (`playback/hwaccel`, autosave, theme, `ui/animations` + `ui/animationSpeed`) |
| `EditBehaviour.h` | convention-dependent editing defaults (`edit/*`: still / generator / transition lengths, three-point editing, ripple delete without selection, trim zones; `migrateSettings()` drops retired keys at start-up); see docs/behavior-spec.md |

## Media pool organisation (bins branch, v2)
- Bins are a tree (`Bin::parentId`); siblings keep the order of `MediaPoolState::bins`.
  `moveBin()` refuses cycles; `removeBinWithContents()` deletes sub-bins and media files,
  timeline items move to Master (Resolve deletes a bin with its contents; timelines are only
  deleted explicitly).
- `MediaItem::clipColor` (palette id), `flags` (bit set, `ClipColors.h`), `metadata`
  (`MediaMetadata`), `addedMs`, `fileModifiedMs`. Timeline clips use `ClipData::label` (a palette
  value) and fall back to the item colour when drawn; flags belong to the item, so every
  instance shows them.
- `MediaPoolState::smartBins` are part of the undo state; default smart bins
  (`smartbins::defaultSmartBins()`, ids `default.*`) are never stored.
- `Project::binViewState()` (view mode + sort per bin key) is saved as "binViews", not
  undoable.
- Usage: a linked V+A pair is one use; compound clip timelines count, so media only inside a
  compound clip is "used".
- `MediaPool::item()` is a hash lookup (index rebuilt lazily after structural changes).

## Proxy / optimized media links and cache settings (proxies branch)
`MediaLinks.h`: `MediaLinks` (owned by the Project, `Project::mediaLinks()`) maps media pool item
ids to a proxy and an optimized media `MediaLink` (path, source fingerprint = size + mtime of
the original, picture size, format). Not part of the undo state; saved as "mediaLinks".
`state()` = None / Ready / Stale (source changed) / Missing; an offline original counts as
Ready. `autoLink()` links files with the stable name `stableLinkFileName()` whose JSON sidecar
names the original. `MediaCacheSettings` (`Project::cacheSettings()`, saved as
"cacheSettings", not undoable, marks the project modified): proxy / optimized resolution and
format, render cache format, background caching, User-mode options, Proxy Handling, Use
Optimized Media, Render Cache mode. `ClipData::cacheOutput` = Render Cache Clip Output.
Preferences: `prefs::proxyDirectory()`, `optimizedMediaDirectory()`,
`renderCacheDirectory()`, `renderCacheLimitGiB()`.

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
  clip time when a clip is retimed (spec 29.10).
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
- An undo command stores the before/after state of every timeline, the `MediaPoolState` and
  the project format. Timelines are stored packed (`UndoSnapshots.h`): a clip-less shell plus
  shared immutable clips, packed against the step below, so unchanged timelines and clips cost
  a pointer (round 4 stress pass: 0.85 -> 0.16 MB per blade on a 1,500-clip timeline); undo
  unpacks with one copy, or reuses the live data when it still exists. Its first `redo()` is a no-op because the change was already applied
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

## Adjustment clips, generators, subtitles (v2)
- `ClipKind::Adjustment`: no source; its effects / transform / masks process the tracks below
  (src/render/readme.md). `ClipKind::Generator`: `ClipData::generator` = descriptor id, its
  parameters in `ClipData::title` (so KeyframeOps paths are `title/<key>`).
- `TrackType::Subtitle` / `TimelineData::subtitleTracks` (drawn above V tracks, ST1 = index 0)
  hold only `ClipKind::Subtitle` clips (an invariant). A caption's text is `keys::Text` in
  `ClipData::title`; `TrackData::subtitleStyle` is the track's default style; with
  `subtitles::OwnStyle` on, the caption's own style keys override it. `editops::addTrack()`
  gives a new subtitle track the default style. Per-type arrays are indexed with
  `trackTypeIndex()` (`kTrackTypeCount` = 3); `detail::kTrackTypes` lists all three, so ripple,
  split, gap and invariant code covers subtitle tracks. Compound clips keep captions in the
  parent.
- `MediaItemKind::Subtitle`: an imported .srt / .vtt, with its parsed `cues` stored in the
  project (`MediaPool::importFiles()` parses instead of probing).

## Invariants (checked by `editops::checkInvariants`)
Clips on a track are sorted by start and never overlap; subtitle tracks hold Subtitle clips
and only they do; `duration >= 1`, `start >= 0`,
`speed > 0`, `sourceInUs >= 0`; every clip/track/effect/mask id is non-zero, unique and `< nextId`;
locked tracks are never changed by EditOps. Ids come from `TimelineData::allocateId()`.

## Project file format (`.kkproj`, JSON)
```json
{ "formatVersion": 7, "generator": "Kinoklip 0.1.0",
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
1 - 3 have none and load unchanged. Format version 7 adds `"kind": "adjustment" | "generator" |
"subtitle"` clips (`"generator"` id), `"subtitleTracks"` (with `"subtitleStyle"`, `"language"`)
and subtitle media items (`"kind": "subtitle"`, `"cues": [[startUs, endUs, text]]`); versions
5 and 6 belong to the retime / proxy branches; all older files load unchanged.
Format version 8 adds multicam: timeline `"multicam": true`, clip `"multicamAngle"`, media
pool item `"kind": "multicam"`, media info `"timecode"` (all optional).
Format version 10 (attrs) adds clip fades and Clip Attributes (see `Serialization.h`).
Format version 14 (guides) adds the optional timeline `"guides"` (`[{"o": "h" | "v", "pos":
px}]`, core/ViewerGuides.h, display only); versions 1 - 13 have none and load unchanged.
Format version 13 (polish4) adds the optional marker `"duration"` (frames, written only when
> 1: ranged markers); every older marker is one frame, so versions 1 - 12 load unchanged.
`Project::captureSave()` / `writeSnapshot()` split saving into a microsecond capture of shared
values (GUI thread) and the JSON build + write (any thread; the background autosave).
Format version 11 (bins): items may carry "clipColor", "flags", "metadata", "added",
"modified"; the media pool "smartBins"; the project "binViews" (see `Serialization.h`).
Versions <= 10 get "modified" (and "added") from the file on disk when loading.
Format version 12 (audiopage) adds the mixer: audio tracks may carry
"mix" (strip, channel format, bus routes, automation mode and curves), timelines "mixer" (buses,
output bus); see `Serialization.h` / `MixerJson.h`. Absent = a neutral mixer, so older files
load and play unchanged.
Format version 15 (compcore) adds the optional clip `"composition"` (Compositing page node
graph, `serialization::compositionToJson()`); versions 1 - 14 have none and load with no
compositions.
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
   (id = stable snake_case string stored in project files); v4 video effects go to
   `registerVideoEffects()` in `BuiltinVideoEffects.cpp` with a library folder
   (`EffectDescriptor::group`, one of `effectgroups::`), their ids and keys in
   `VideoEffectKeys.h` (the second batch: `registerVideoEffects2()` in
   `BuiltinVideoEffects2.cpp`; the 1.2 batch: `registerVideoEffects3()` in
   `BuiltinVideoEffects3.cpp`; its second part, spec 57: `registerVideoEffects4()` in
   `BuiltinVideoEffects4.cpp`, keys in `VideoEffectKeys4.h`; the multi-frame effects, spec 60:
   `registerVideoEffects5()` in `BuiltinVideoEffects5.cpp`, keys in `VideoEffectKeys5.h`; the template
   looks, spec 65: `registerVideoEffects6()` in `BuiltinVideoEffects6.cpp`, keys in `VideoEffectKeys6.h`; the tracking effects, spec 70: `registerVideoEffects7()` in
   `BuiltinVideoEffects7.cpp`, keys in `VideoEffectKeys7.h`, the stored track in `EffectTrack.h`,
   Match Move's maths in `MatchMove.h`, Surface Tracker's in `SurfaceTracker.h`).
   Builders: `EffectBuilders.h`.
2. Implement the pixels in `src/render` keyed by the same id. The Inspector picks it up
   automatically from the descriptor; a Color parameter (or a Double hue) with `viewerPicker`
   gets the viewer eyedropper (`SelectionModel::colorPicker()`).
3. Parameter types: Double / Int (keyframable), Bool, Color, Choice, String, **Curve** (tone
   curve points as text, `CurveParam.h`: `parse()`, `format()`, monotone `evaluate()`; edited
   with the Inspector's curve graph) and **File** (an absolute path; `FileParams.h` stores it
   in project files as {file, relative} so a moved project folder finds it again;
   `ParamDescriptor::fileFilter` is the dialog filter). Every non-scalar type is a static.

**A new Compositing page node:**
1. Id, ports, parameters (`ParamDescriptor`, units as documented on `NodeDescriptor`) and
   viewer controls in the `NodeRegistry` constructor (`NodeRegistry.cpp`), keys in
   `nodekeys::`. A new Edit page video effect needs nothing here: it becomes an `fx:<id>`
   node automatically.
2. Its pass in `render/GlComposition.cpp` (`evalNode()`), the CPU twin in
   `render/CpuComposition.cpp`, shared formulas in `render/CompMath.*`.
3. Tests: `tests/core/test_composition.cpp` (registry, ops) and
   `tests/render/test_comprender.cpp` (pixels, GPU vs CPU).

## Compositing page (`Composition.h`, `NodeRegistry.h`, `CompOps.h`; spec section 42a)
- `ClipData::composition` (std::optional): the clip shows the composition's output (the image
  arriving at MediaOut) instead of its picture; its Edit page attributes apply on top
  (src/render/readme.md, "Compositions"). Keyframes in it are clip-local (comp frames), so
  trims and splits shift them like every clip keyframe (`shiftClipKeys()`); copy / paste,
  duplicate, compound and nesting copy the clip value and keep it.
- Every change goes through `Project::setClipComposition(timelineUid, clipId, comp, label)`:
  one named undo step (or part of an open edit, e.g. a slider drag), in the active or any
  other timeline. The UI builds the new value with `CompOps` first.
- MediaIn nodes reading another media pool item count as a use of that item (`MediaUsage`);
  File parameters of effect nodes are stored relative to the project like clip effects.

**A new field in ClipData/TrackData/TimelineData:**
1. Add it with a default and a `///` comment (units!).
2. Write/read it in `Serialization.cpp` (missing on read → default, so old files keep loading).
3. Extend `richTimeline()` in `tests/core/test_serialization.cpp` so the round-trip covers it.
4. Bump `kFormatVersion` only for incompatible changes, and add a migration in `Project::load()`.

## Fades, level lines, Clip Attributes (attrs branch, spec section 31)
- `ClipData::fadeInFrames` / `fadeOutFrames`: fade handles (clip properties, not keyframes);
  `videoFadeFactor()` (linear) and `audioFadeGain()` (equal power) in `ClipAttributes.h` are the
  one formula renderers and mixer use; `fadeInCurve` / `fadeOutCurve` (-1 ... +1, 0 = linear,
  spec section 59) bend the progress through `fadeShape()` first. Splits / range cuts drop the fade at the cut
  (`EditOpsInternal.h`), joins keep the outer ones.
- `LevelOps.h` (namespace `kk::levelops`): the edits behind the timeline's level lines and fade
  handles (offset the volume at a frame, add / move volume keys, set a fade), callable without
  the mouse.
- `ClipAttributes.h`: pixel aspect, data levels, field order + deinterlace, extra rotation,
  channel format / routing (`channelMatrix()`), start timecode. The truth is
  `MediaItem::attributes` (`MediaPool::setItemAttributes()` inside an edit);
  `Project::syncDerived()` mirrors it into `ClipData::attributes` of every Media clip of the
  item in every timeline, so render snapshots carry it. Serialized from format 10.
- Stacked timelines: `Project::setTabs()`, `timelinePlayhead()` / `setTimelinePlayhead()`
  (inactive timelines keep their own playhead; `inactivePlayheadChanged`).

## Mixer model (Audio page, spec section 37)

`MixerData.h`: `ChannelStrip` (6-band `EqSettings`, `DynamicsSettings`, fader dB, pan, bus mute),
`TrackMix` on every `TrackData` (strip, mono / stereo channel format, bus routes, `AutomationMode`
off / read / touch / write, fader and pan automation curves keyed in **timeline frames**),
`BusData` and `MixerData` on every `TimelineData` (buses, the output bus, the id allocator; Bus 1
always exists). Value types, so undo, render snapshots and copy / paste carry them. `MixerOps.h`
(`kk::mixerops`): pure edits (add / rename / delete buses, routes, channel format, output bus,
automation mode, automation writing with erase-what-was-passed, clamping) and the evaluation the
mixer and the widgets share (`faderDbAt()`, `panAt()`), callable without the mouse. `MixerJson.h`:
format 12 (only non-default values are written; the loader repairs missing main buses,
duplicate ids, unknown routes and out-of-range values). The track's `muted` / `solo` flags are the
timeline header's (one state for the header and the mixer). Track automation does not move with
ripple edits (it belongs to the timeline, not the clips; the clip level line keeps using the
clip's `audio/volumeDb` keys).

