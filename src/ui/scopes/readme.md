# src/ui/scopes — video scopes

Reusable scope widgets (docs/color-page.md "Scopes"); the data comes from `render/Scopes.h`.

- **ScopeView**: draws one scope (waveform luma / RGB, parade, vectorscope, histogram) of a
  QImage with its graticule.
- **ScopesPanel**: a ScopeView with its type menu (remembered under a settings key).
- **ScopeFeeder**: renders the program frame small (`PlaybackEngine::renderImage()`, graded,
  following "bypass all") after every presented frame, one request in flight.
- **ScopesWindow**: View > Video Scopes (Ctrl+Shift+W), two scopes in a floating tool window
  on any page.

Used by the Color page (`src/app/pages/ColorPage`) and the scopes window (MainWindow).
