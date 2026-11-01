# src/ui/effects — effects library

**Status: placeholder from the foundation agent; the panels agent owns this module.**

`EffectsLibraryPanel` lists `EffectRegistry::instance()` descriptors by category and emits
`effectActivated(id)` on double-click.

## How to extend
Add drag & drop (mime type carrying the descriptor id) onto timeline clips/cuts, a search field
and favourites. New effects appear automatically once registered in
`src/core/BuiltinEffects.cpp`.
