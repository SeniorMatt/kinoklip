# Owner feedback on 0.1.0-alpha.1 (tested on T14s Gen 2, KDE Linux, Wayland)

The screenshots named below are kept only on the development machine (`local/feedback/`, not in the
repository): they show private files and third-party images.

Screenshots: alpha1-2.png (light/system scheme after switching: half-switched UI, menu arrow
overlapping "Color Theme"), alpha1-3.png (submenu arrow overlaps text), alpha1-4.png (Fit/Auto
combos cramped, no padding), alpha1-5.png (Deliver preset strip: clipped labels "YouTube ... 1080",
odd padding/scrollbar), alpha1-6.png (Preferences: odd group title placement/padding, wrapped hw list).

1. Switching colour scheme breaks the UI and sometimes segfaults. Launcher: wayland platform plugin
   missing in bundle; portal could not register app id org.kinoklip.Kinoklip.
2. Small visual inconsistencies (arrow on text, paddings) - fix everywhere.
3. Everything must act like DaVinci Resolve, and the UI must look as close to Resolve as possible;
   Breeze only as the widget theme. Not a KDE app with Resolve features: Resolve built in Qt.
4. Missing: ripple cut and ripple delete (most used, Resolve defaults).
5. Keyframes: needs a separate keyframe editor; behave like Resolve.
6. Mouse-wheel zoom: Resolve zooms around the playhead; make it a setting (playhead / mouse cursor).
7. Missing settings, e.g. render resolution and the rest of Resolve's render/project settings.
8. Shortcuts: copy all Windows Resolve defaults (e.g. split is Ctrl+\, not Ctrl+B); keymap profiles
   system with switching (only the Windows Resolve profile needed now).
9. Render settings panel padding/layout off.
10. Positive: basic cutting, moving and rendering worked; performance fine.
