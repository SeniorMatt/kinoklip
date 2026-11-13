# Bug hunt for 1.7: what is left

Round of 2026-11-13 over the five classes the owner found on 2026-11-12 (viewer not following,
mouse buttons, small screens, keys taking over, missing search and defaults), plus Resolve
parity. The fixes are spec section 89. This list is what was found and not fixed. Line numbers
are of the branch at the end of the round. Resolve behaviour is from its public manual only.

The probes that found most of this stay in `tests/app/test_bughunt12.cpp` (`probeLayouts`,
`probeKeyConflicts`, `probeKeyStealing`, `probeTabOrder`; `KK_PROBE_ALL=1` opens every panel
first). They print `PROBE`, `TEXTPROBE`, `KEYPROBE`, `STEALPROBE` and `TABPROBE` lines.

## What the probes found clean

- 1366 x 768: no two buttons or combo boxes overlap on any page, in the default layouts. The
  "text cut" lines of `probeLayouts` are icon buttons whose text is only the tooltip.
- Key clashes: every shared key of the registry is separated by scope (`widget:node_editor`,
  `widget:color_page`, the Media Pool); the global I, O, M, J, K, L, Space, arrow and Delete
  keys are not claimed by any focusable widget on the Edit page (the Media Pool lists, the
  Color page and the node editor claim theirs on purpose, spec 33.31).
- Tab order of Preferences, Project Settings, New Project and Normalize Audio, and of the
  Edit, Color, Compositing, Deliver and Audio pages, follows the layout (the Deliver
  "RenderUseOptimized" line is a scrolled widget below the fold, not a bug).

## Fixed in round 2 (spec 89.12 - 89.19)

- Keys: mask overlay and keyframe views claim Delete, Backspace, Enter and Escape only without a
  modifier; the Spline keys S, L, T, I, O, V and F work only in the Compositing page's Spline panel.
- Small screens: Effects library width floor, Cut page tool bar as a QToolBar with "»", LUT
  browser tool row in a QToolBar with "»" and the interpolation on its own row.
- Search: the transition Type list in the Inspector is searchable.
- History: folder history and mouse back / forward in the Media Storage browser.
- Clip Attributes: Video frame rate override and Alpha mode.
- Create New Timeline: Empty Timeline, Use Project Settings, audio track type (Stereo, Mono).
- Edit Index: decided to have no row context menu (89.19).

## Not fixed

- `src/ui/common/ListKeyGuard.cpp:80-120`: check boxes, radio buttons, sliders and tab bars
  take no keys, so Space (play) and Left / Right (frame step) win over them when they have the
  focus. Resolve does the same for Space; sliders and tab bars are open.
- `src/core/Preferences.h:18`: autosave every 2 minutes; Resolve's default for Live Save is
  different (check its manual before changing).
- Frame rate override: clips already on the timeline keep their speed when the attribute
  changes; the Cut page source tape (`src/core/SourceTape.cpp`) ignores the override; the alpha
  mode is not applied by the CPU renderer.
- Create New Timeline: audio track types 5.1, 7.1 and Adaptive (the mixer has mono and stereo
  tracks only).
- `src/ui/deliver/RenderSettingsPanel.cpp:301-307`: Individual clips is a disabled radio
  button.
- `docs/effects-library-status.md`: the entries marked `planned` (ACES Transform, Color Space
  Transform, DCTL, Depth Map, Face Refinement, Relight, Object Removal, Sky Replacement, Text+
  and others).
- Timeline tabs (`src/ui/timeline/TimelineTabs.cpp:173`): Rename Timeline is in the menu only;
  check whether Resolve renames on a double-click on the tab before adding it.
