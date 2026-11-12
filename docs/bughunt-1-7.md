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

## Not fixed

### Keys

- `src/ui/viewer/MaskOverlay.cpp:619-640` (ShortcutOverride) and `:750-780` (keyPressEvent):
  Delete, Backspace, Enter and Escape are claimed with any modifier. Shift+Delete and
  Ctrl+Backspace should stay global. Needs a mask in edit mode to test.
- `src/ui/keyframes/KeyframeViewBase.cpp:411-421`: Backspace is claimed with any modifier
  while keys are selected.
- `src/ui/keyframes/KeyframesPanel.cpp:488-497`: the panel keys S, L, T, I, O, V, F are
  active in the Keyframes panel of the Edit page too, so after a click there I is Step In and
  not Mark In, and L is Linear and not shuttle forward. Spec 87.7 chose this; Resolve's Edit
  page keyframe editor has no such keys. Option: enable them only in the Spline panel of the
  Compositing page (`isCompositionPanel()`), leave the tool row and menus for the Edit page.
- `src/ui/common/ListKeyGuard.cpp:80-120`: check boxes, radio buttons, sliders and tab bars
  take no keys, so Space (play) and Left / Right (frame step) win over them when they have the
  focus. Resolve does the same for Space; sliders and tab bars are open.

### Small screens

- `src/ui/effects/EffectsLibraryPanel.cpp` (tree, created around line 400): with the Media
  Pool, Effects and Edit Index all shown, the library tree gets 58 px at 1366 px and every
  transition name is elided to a few letters (`KK_PROBE_ALL=1 test_bughunt12 probeLayouts`).
  Give the left column a width floor, or fold Edit Index into the same tab stack.
- `src/ui/cut/CutToolBar.cpp:50-100`: a plain `QHBoxLayout` of about twenty buttons and the
  message label; its minimum width decides the Cut page's minimum window width. Fold it into a
  `QToolBar` with the » menu as the Compositing node row does (`CompositingPage.cpp`
  `buildToolBar`).
- `src/ui/color/ColorPalettes.cpp` (LutBrowser constructor): the tool row (`Add Folder`, `Load File`, `Refresh`,
  `Interpolation` combo, `Remove LUT`) is one `QHBoxLayout` row; check it at the narrowest
  Color page palette width.

### Search and defaults

- `src/ui/inspector/InspectorPanel.cpp:1241`: the transition Type combo lists every transition
  (about sixty) without a search; Resolve's list is also long. A searchable popup like the
  Add Effect menu would do.
- `src/ui/comp/NodeEditor.cpp:792-805`: the Add Node context menu is submenus only; Shift+Space
  (Select Node) is the search, but the menu does not say so. Add the entry at its top.
- `src/ui/mediapool/MediaBrowserPanel.cpp:setRootPath`: the Media Storage browser has no folder
  history, so the mouse back / forward buttons do nothing there (the Project Manager has them
  since 89.8).
- `src/core/Preferences.h:18`: autosave every 2 minutes; Resolve's default for Live Save is
  different (check its manual before changing).

### Resolve parity (missing controls)

- `src/ui/dialogs/ClipAttributesDialog.cpp:113-190`, `src/core/ClipAttributes.h:47`: no
  **Video frame rate** override (Resolve's Clip Attributes > Video lets a 60 fps file play as
  24 fps) and no **Alpha mode** (Auto, Premultiplied, Straight, None).
- `src/ui/deliver/RenderSettingsPanel.cpp:301-307`: Individual clips is a disabled radio
  button.
- `src/ui/timeline/TimelineDialogs.cpp:200-245`: the Create New Timeline dialog has start
  timecode, name and track counts; Resolve also has **Empty Timeline**, **Use Project
  Settings** and the audio track type (Mono, Stereo, 5.1, 7.1, Adaptive).
- `docs/effects-library-status.md`: the entries marked `planned` (ACES Transform, Color Space
  Transform, DCTL, Depth Map, Face Refinement, Relight, Object Removal, Sky Replacement, Text+
  and others).
- Edit Index (`src/ui/editindex/EditIndexPanel.cpp`) has no context menu (check what
  Resolve offers on its marker list before adding one).
- Timeline tabs (`src/ui/timeline/TimelineTabs.cpp:173`): Rename Timeline is in the menu only;
  check whether Resolve renames on a double-click on the tab before adding it.
