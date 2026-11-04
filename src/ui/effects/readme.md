# src/ui/effects — effects library

Owned by the panels agent.

| File | What |
|---|---|
| `EffectsLibraryPanel.h` | sectioned tree (Video Transitions, Titles, Generators, Effects, Video Effects, Audio Effects) with a search field, painted category glyphs, drag source |
| `EffectActions.h` | undoable operations: `applyEffect()`, `applyTransition()`, `removeTransition()`, `addGeneratorClip()`, `activate()` (double-click semantics) |
| `EffectsLivePreview.h` | Live Preview: `livepreview::buildScene()` (the hovered entry applied to a private timeline copy, and which frame to show) and `EffectsLivePreview` (debounced, cancellable renders shown with `ViewerPanel::showTemporaryFrame()`) |

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
- **Live Preview** (spec 47; options menu "…" > Live Preview, on by default, Preferences key
  `effects/livePreview`): while the pointer rests on an entry the Edit page's timeline viewer
  shows the frame with it applied; nothing is written to the project.
  - video effects: default parameters on the topmost selected clip under the playhead, else
    the topmost clip there;
  - video transitions: on that clip's end nearer to the playhead (an existing transition
    keeps its length and alignment, else 1 s centred on a cut); the viewer shows a frame of
    the transition, chosen by the pointer's place along the row (left = start, middle =
    midpoint, right = end), so the playhead is not moved;
  - titles and generators: laid over the frame on a new top track; the pointer's place along
    the row scrubs through the title (the playhead's picture stays underneath);
  - nothing for audio entries and the Adjustment Clip.
  A new entry renders after 100 ms of rest; moving along the same entry re-renders at once
  (one render in flight). Leaving the entry or the list, scrolling, a drag, a search, hiding
  the panel, playback, any edit, a timeline switch end it; a playhead move or a selection
  change re-render it. Renders go through `PlaybackEngine::renderAlternative()` (the
  viewer's size, quality and proxies, on the render thread); a stale request is cancelled.
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
- **Live Preview of a new category:** add a case to `livepreview::buildScene()` that returns
  the modified copy and the frame to show (std::nullopt = no preview).
- **Live Preview elsewhere** (e.g. the Compositing page's tool list): create an
  `EffectsLivePreview` for that page's viewer and feed it hover() / stop(); the scene
  builder would need a composition-node case.
- **Favourites:** keep a QSettings string list of ids and add a "Favourites" section at the
  top of `populate()` (not implemented yet).
