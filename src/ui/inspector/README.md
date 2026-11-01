# src/ui/inspector — clip inspector

**Status: placeholder from the foundation agent; the panels agent owns this module.**

`InspectorPanel` shows tabs (Video / Audio / Effects / Title / Transition) and follows
`SelectionModel::currentClip()`.

## How to extend
Build editors generically from `EffectRegistry` descriptors: the `ClipProperties` groups
(`groups::Transform`, `groups::Cropping`, `groups::Audio`; their `storage` field names the
`ClipData` member), `text_title` for titles, `solid_color` for solids, and each
`EffectInstance::type`. Scalars live in `ParamSet::values` (keyframeable, clip-local frames),
everything else in `ParamSet::statics`. Edit through `beginEdit()/endEdit()`; connect
`clip.add_keyframe/prev_keyframe/next_keyframe`.
