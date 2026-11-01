# src/ui/deliver — Deliver (render) page

**Status: placeholder from the foundation agent; the export agent owns this module.**

`DeliverPage` (a `cw::Page`, id `deliver`): Render Settings | timeline `ViewerPanel` |
Render Queue, over a mini timeline. Splitter state persists via `saveLayout/restoreLayout`.

## How to extend
Replace the placeholder panels with the render settings form (presets from `src/export`), the
render queue (progress, ETA, cancel) and an in/out range bar. Keep the constructor signature
`DeliverPage(AppContext&, QWidget*)` — `MainWindow` creates it.
