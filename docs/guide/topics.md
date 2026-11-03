# Guide topics: the plan

What beginners ask most when they start editing in a Resolve-style editor, ranked, and whether
Kinoklip can do it today. This list is the plan for the guide's task articles; a topic gets an
article when it is written (linked below).

How the list was made (November 2026): public beginner questions about Resolve's Edit, Cut and
Deliver pages, from the Blackmagic forum's beginner threads and user-written FAQ, the
r/davinciresolve wiki and "FAQ Friday" threads, and the many "beginner questions" tutorial
pages that answer them. Only the topics were taken; no text or pictures (see
[clean-room.md](../clean-room.md)). Ranks are a judgement of how often each question comes up,
not a count.

| # | Topic | Why it is common | Article |
|---|---|---|---|
| 1 | Export a video for YouTube | Every new editor has to get the video out; the Deliver page with its many settings is the first wall. | [Export a video for YouTube](export-for-youtube.md) |
| 2 | Cut a clip and remove a part | The first real edit: blade, split at the playhead, delete with or without closing the gap. | [Getting started](getting-started.md#cut-trim-and-delete) |
| 3 | Remove a green screen | The best-known "effect" question; people expect a one-click key and get stuck on edges and green spill. | [Remove a green screen](remove-a-green-screen.md) |
| 4 | Speed up or slow down a clip | Slow motion and time-lapse are asked about constantly, including ramps and why the audio sounds odd. | [Change the speed of a clip](change-clip-speed.md) |
| 5 | Add music and fade audio in and out | Background music and a smooth fade at the end are part of almost every video. | [Fade audio and add music](fade-audio-and-add-music.md) |
| 6 | Add text and titles | Captions, lower thirds and an opening title; people look for "the text tool". | [Getting started](getting-started.md#add-a-title) |
| 7 | Add a transition between clips | Cross dissolves and where to find the other transitions. | [Getting started](getting-started.md#add-a-transition) |
| 8 | Fade the picture to or from black | The ending of nearly every video; often confused with transitions. | planned (fade handles, spec 31.4) |
| 9 | Freeze a frame | Holding one frame for a while, for comedy or emphasis. | planned (Shift+R, spec 29.8) |
| 10 | Zoom, crop or make a picture-in-picture | Transform and Cropping in the Inspector, on-screen controls. | planned |
| 11 | Choppy playback | Large or long-GOP files play badly; the answers are proxies, optimized media, playback resolution and the render cache. | planned |
| 12 | Make a vertical video (9:16) for Shorts, Reels and TikTok | Timeline resolution versus render resolution confuses many people. | planned (Project Settings > Timeline resolution) |
| 13 | Blur or hide part of the picture (a face, a number plate) | A mask plus a blur on a copy of the clip. | planned (masks, spec 27; Gaussian Blur) |
| 14 | Add subtitles | Importing .srt files, typing captions, burning them in. | planned (spec 28) |
| 15 | Separate the audio from the video | Unlinking a clip to move or delete only its sound. | planned (Clip > Unlink Clips) |
| 16 | Animate with keyframes (a slow zoom, a moving title) | The step after basic edits. | planned (spec 15) |
| 17 | Make quiet and loud clips the same volume | Normalize Audio Levels and the clip volume. | planned (Clip > Normalize Audio Levels) |

Topics that come up often but that Kinoklip cannot do yet (no article until it can):

- needs: video stabilisation (no stabiliser effect yet).
- needs: motion tracking (to make a blur or a title follow a moving object).
- needs: automatic subtitles from speech (speech-to-text).
- needs: noise reduction for audio (voice clean-up); the Noise Reduction in the effects library is for video.
- needs: Difference Key against a clean plate (spec 40.10), for screens that are not green or blue.
- needs: rendering every timeline clip to its own file (Deliver > Individual clips is disabled).
