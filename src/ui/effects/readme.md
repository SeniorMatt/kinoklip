# src/ui/effects — effects library

Owned by the panels agent.

| File | What |
|---|---|
| `EffectsLibraryPanel.h` | sectioned tree (Video Transitions, Titles, Generators, Effects, Video Effects, Audio Effects) with a search field, painted category glyphs, drag source |
| `EffectActions.h` | undoable operations: `applyEffect()`, `applyTransition()`, `removeTransition()`, `addGeneratorClip()`, `activate()` (double-click semantics) |

## Behaviour
- **Drag:** every entry exports `mime::Effect` (`ui/common/DragMime.h`) with the descriptor id
  as UTF-8 payload. Drop targets: the timeline (timeline agent) and the inspector.
- **Double-click / Enter** calls `effectactions::activate()`:
  - video/audio effects are appended to every selected clip whose track type fits,
  - transitions go onto the current clip, at the end nearer to the playhead (1 s default,
    clamped to the clip; retyping keeps the length),
  - titles, generators and the Adjustment Clip (section "Effects") become a new clip at the
    playhead (Preferences > Editing generator duration, 5 s by default), on the track above the
    highest video track that is busy there (a track is added if needed); it gets selected.
  Every call is exactly one undo step, and a no-op creates none.
- **Folders:** Video Effects are grouped by `EffectDescriptor::group` into folders (Blur,
  Color, Distort, Key, Light, Stylize, Transform), sorted by their translated names; effects
  without a folder follow them directly. The Inspector's Add Effect menu uses the same folders.
- **Search** filters by name and description (case-insensitive), hiding empty sections and
  folders.

The timeline agent can reuse `effectactions::applyEffect/applyTransition/addGeneratorClip`
for its drop handling so semantics stay identical.

## How to extend
- **New effect / transition / title / generator:** register its descriptor in
  `src/core/BuiltinEffects.cpp`; it appears here automatically in the matching section.
- **New category:** add it to the section list in `EffectsLibraryPanel::populate()`, give it a
  title in `sectionTitle()` and a glyph in `categoryGlyph()`, and define what activation does
  in `effectactions::activate()`.
- **Favourites:** keep a QSettings string list of ids and add a "Favourites" section at the
  top of `populate()` (not implemented yet).
