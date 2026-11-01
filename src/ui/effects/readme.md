# src/ui/effects — effects library

Owned by the panels agent.

| File | What |
|---|---|
| `EffectsLibraryPanel.h` | sectioned tree (Video Transitions, Titles, Generators, Video Effects, Audio Effects) with a search field, painted category glyphs, drag source |
| `EffectActions.h` | undoable operations: `applyEffect()`, `applyTransition()`, `removeTransition()`, `addGeneratorClip()`, `activate()` (double-click semantics) |

## Behaviour
- **Drag:** every entry exports `mime::Effect` (`ui/common/DragMime.h`) with the descriptor id
  as UTF-8 payload. Drop targets: the timeline (timeline agent) and the inspector.
- **Double-click / Enter** calls `effectactions::activate()`:
  - video/audio effects are appended to every selected clip whose track type fits,
  - transitions go onto the current clip, at the end nearer to the playhead (1 s default,
    clamped to the clip; retyping keeps the length),
  - titles and generators become a new 5 s clip at the playhead, on the track above the
    highest video track that is busy there (a track is added if needed); it gets selected.
  Every call is exactly one undo step, and a no-op creates none.
- **Search** filters by name and description (case-insensitive), hiding empty sections.

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
