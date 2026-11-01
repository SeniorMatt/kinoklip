# resources — compiled-in assets

| Path | What |
|---|---|
| `resources.qrc` | list of every compiled-in file (CMake compiles all `resources/*.qrc` into `kinoklip_core`) |
| `keymap_default.json` | default shortcuts per action id (PLAN §5.7); user overrides live in QSettings |
| `icons/*.svg` | the app's own 24×24 single-colour icons, tinted at runtime by `kk::icon()` |
| `icons/app.svg` | coloured application icon (not tinted; packaging may replace it) |

Resources live in a static library, so `kk::initResources()` must be called once (main and
tests) — see `src/ui/common/Resources.h`.

## How to extend
- **Icon:** draw on a 24×24 grid with black strokes (`stroke-width` ≈ 1.75, round caps/joins) or
  black fills; keep it simple and consistent with the others. Add a `<file>icons/x.svg</file>`
  line to `resources.qrc`.
- **Default shortcut:** add `"<action id>": ["Ctrl+Something"]` to `keymap_default.json`.
