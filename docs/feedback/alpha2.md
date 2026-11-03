# Owner feedback on 0.1.0-alpha.2

The screenshots named below are kept only on the development machine (`local/feedback/`, not in the
repository): they show private files and third-party images.

Screenshots: alpha2-7.png (our Import Media dialog: Qt's own widget dialog), alpha2-8.png (the KDE /
Dolphin file dialog the owner wants instead), alpha2-9.png (viewer during playback: very pixelated),
alpha2-10.png (viewer paused: sharp; red arrow at the top-right of the timeline viewer header, where
Resolve has its viewer-mode control), alpha2-11.png (a dissolve on the timeline that cannot be clicked).

1. Import (and every other file dialog) should be the standard KDE/Dolphin file chooser.
2. BIGGEST CONCERN: picture quality during playback is very low (pixelated); paused it is sharp.
   The owner could not find a setting for it (the "Auto" combo in the viewer header was not
   recognised as a quality setting).
3. Moving the playhead manually while playing lags.
4. Ripple cut / delete "don't work". The owner uses Resolve's Ctrl+Shift+[ and Ctrl+Shift+]
   (ripple trim start / end of the clip to the playhead) - not mapped/working in Kinoklip.
5. No button to leave the dual (source + timeline) viewer mode.
6. Transitions on the timeline cannot be clicked/selected.
7. Colour-scheme switching: fixed, no crashes.
8. Mouse wheel behaviour (Resolve mapping: Alt+wheel zoom etc.): owner likes it, keep as default.
9. Ripple trim works on Shift+[ / ] already; owner wants Resolve's mapping: Ctrl+Shift+[ / ] =
   ripple trim to playhead, Shift+[ / ] = trim leaving a gap (sent to editfix).
10. Full keyframe editor window like the latest Resolve (alpha2-12.png: the inline curve editor under
    a short clip is far too cramped to use). A large dockable/floating Keyframes panel synced with
    the timeline, with dope-sheet rows and a big curve view for the selected clip(s). Queued after
    the current round (it touches the keyframe/inspector UI).
11. Selecting a video clip with linked audio makes the inspector show the audio part ("2 clips
    selected, showing the last one", Video tab empty), so scale can't be changed (alpha2-13.png).
    Resolve treats a linked pair as one inspector item (Video tab = video part, Audio tab = audio
    part). Sent to editfix.
