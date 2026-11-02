# src/ui/inspector — clip inspector

Edits the parameters of the selected clip. Owned by the panels agent.

## Architecture

| File | What |
|---|---|
| `InspectorPanel.h` | the panel: header, five tabs (Video / Audio / Effects / Title / Transition), structure tracking, undo protocol (implements `ParamEditHost`) |
| `ParamGroupBox.h` | one collapsible section (Transform, Cropping, an effect instance, a transition) with a shared `QGridLayout` |
| `ParamRows.h` | one editor row per `ParamDescriptor`: number, bool, choice, colour, string, title text, font |
| `InspectorMasks.cpp` | the Video tab's Masks section (part of `InspectorPanel`): add buttons per shape, one group per mask (enable, edit in the viewer = `SelectionModel::setActiveMask()`, reorder, delete, section diamond, shape row), rows from the `builtin.mask` descriptor with `ParamScope::Mask` |
| `ParamAccess.h` | `ParamRef` (clip, scope, effect id, key) and the read/write functions that map a descriptor parameter onto `ClipData` |
| `ParamEditHost.h` | the interface rows use to read clips and make undoable edits (`isMixed()`: the selected clips differ) |
| `MultiClipEdit.h` | multi-clip editing (spec 24.1): the peers of the shown clip, effect matching by type and position, copying a change onto the peers (`InspectorPanel::modifyClip()` calls it inside the open edit) |
| `ValueField.h` | draggable numeric field (drag to scrub, Shift fine, Ctrl coarse, double-click to type) |
| `KeyframeButton.h` | keyframe diamond (outlined = animated, filled = key at playhead) + prev/next arrows |
| `InspectorGlyphs.h` | small painted glyphs (reset, remove, up/down, add, chevrons) |

### Data flow
- Everything is generated from `EffectRegistry` descriptors:
  - Video tab: `solid_color` (Solid clips), `builtin.transform`, `builtin.cropping`, then the
    Masks section (`builtin.mask` rows per mask; width / height only for rectangles and
    ellipses; mode / invert map to `MaskData` fields, the synthetic `shape` key to the
    animated path, see `ParamAccess.h`).
  - Audio tab: `builtin.audio`, bound to the clip itself (audio track) or its first linked
    audio clip (a video clip with linked audio).
  - Effects tab: one group per `EffectInstance` (enable, move up/down, remove, reset all) and an
    "Add Effect" menu listing all descriptors of the matching category.
  - Title tab: every `Title` descriptor with `storage == "title"`.
  - Transition tab: head/tail transitions with a type combo, a synthetic `Duration` row
    (`paramaccess::kTransitionDuration`) and the descriptor params.
- **Rebuild vs refresh.** `InspectorPanel::Structure` (clip id, linked audio clip, kind, track
  type, effect ids in order, transition types) decides whether widgets must be rebuilt. Any
  other change (values, keys, playhead) only calls `ParamRow::refresh()`, which is O(1) per row
  and skips unchanged values. `rebuildCount()` lets tests check this.
- **Undo.** Rows never touch `Project` directly. One-shot edits use `ParamEditHost::editClip()`
  (begin + mutate + end = one undo step). Drags (value field, slider) call
  `beginParamEdit()` on press, `modifyClip()` per move (live `notifyChanged()`), and
  `endParamEdit()` on release. Title text: one step per typing burst (closed after 800 ms idle
  or on focus-out).
- **Keyframes.** The diamond toggles a key at the playhead's clip-local frame (clamped into the
  clip; disabled when the playhead is outside). When a scalar is animated, editing its value
  sets a key at the current frame (`AnimatedValue::setValue`). Linked zoom (`zoomLinked`)
  applies value and key edits to both zoom axes. Transition parameters are not keyframed.
  Right-click on a filled diamond picks the key's preset (linear, ease in / out / in and out,
  smooth, static). Sections with animatable parameters (Transform, Cropping, Audio, Title)
  have a section diamond in the header that keys / unkeys all of them at once. The timeline's
  keyframe and curve editors edit the same keys (src/ui/timeline/KeyframeLanes.h).
- **Shortcuts.** `clip.add_keyframe` toggles a key on the parameter the user touched last;
  `clip.prev_keyframe` / `clip.next_keyframe` jump across every animated parameter shown.
- **Drops.** `mime::Effect` dropped on the inspector applies an effect to the shown clip, or a
  transition to the end nearer the playhead.

## How to extend

**A new effect / parameter:** add it to the descriptor in `src/core/BuiltinEffects.cpp`. The
inspector shows it automatically with the editor matching its `ParamType`.

**A new parameter type (e.g. a 2D point or a curve):**
1. Add the `ParamType` value in `src/core/EffectRegistry.h` (core owner) and define where it is
   stored (`values` or `statics`).
2. Add read/write helpers to `ParamAccess` if it is not a plain scalar/static.
3. Write a `ParamRow` subclass in `ParamRows.cpp`: create widgets in the constructor (put them
   into `paramgrid::` columns of the shared grid), implement `refresh()` cheaply (compare
   before setting, block signals), and write through `m_host.editClip()` (one step) or
   `beginParamEdit()/modifyClip()/endParamEdit()` (drags).
4. Return it from `ParamRow::create()`.

**A specialised editor for one key** (like the font combo for `keys::Font`): branch on
`desc.key` in `ParamRow::create()`.

**A new tab section:** add a `Structure` field if its presence changes the widgets, then build
it in `InspectorPanel::rebuild()` with `addDescriptorGroup()` or `addGroup()` + `addParam()`.

## Generators, adjustment clips and captions (v2)
- Generator clips: the generator's descriptor group (parameters in `ClipData::title`,
  `ParamScope::Title`) at the top of the Video tab, then Transform / Cropping / Masks.
- Adjustment clips: Transform, Cropping, Masks and the Effects tab like any video clip.
- Subtitle clips: the Title tab is renamed "Captions" and holds the `builtin.caption` group
  (text, Custom Style), the caption's own `builtin.subtitle_style` group when Custom Style is on
  (switching it on copies the track style into the caption), and the track's default style.
  The track style is edited through a stand-in clip: a virtual clip id (bit 63 + track id)
  that `clip()` / `modifyClip()` map to `TrackData::subtitleStyle`, so the generic rows work
  unchanged.
