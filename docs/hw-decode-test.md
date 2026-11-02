# Hardware decoding test on the T14s (Iris Xe, iHD, Plasma 6 Wayland)

The alpha.3 log showed `[AVHWFramesContext] Failed to sync surface 0x3: 1 (operation failed)`
and `kk.media: hardware frame download failed: "Input/output error"`, and no
`kk.playback: stats` lines. The next build changes four things (see `src/media/readme.md`,
"Hardware decoding robustness"):

* all decoders that share the VAAPI device now take turns for every driver call (the new
  decode-ahead thread, parallel decode, thumbnails and export used the one VADisplay at the
  same time before);
* a failed GPU download is retried once, then that decoder continues in software on the same
  frame (no dropped or garbage frame), with one warning saying why;
* the log says which decoder each file uses, which VA driver and libva are loaded, and probing
  for NVIDIA/QSV no longer prints errors on a machine without them;
* `KK_PLAYBACK_STATS=1` now also prints a summary every time playback stops (alpha.3 printed
  only after one full second of uninterrupted playback, so short plays printed nothing).

Please run the steps below and send `kinoklip-*.txt` back. Each run takes about two minutes.

## 0. System facts (once)

```sh
vainfo 2>&1 | tee kinoklip-vainfo.txt                    # package libva-utils if missing
vainfo --display drm --device /dev/dri/renderD128 2>&1 | tee -a kinoklip-vainfo.txt
ls -l /dev/dri; id                                       # render node permissions / groups
ffprobe -hide_banner your-test-clip.mp4 2>&1 | tee kinoklip-clip.txt
```

In `vainfo` look for `Driver version: Intel iHD driver ... - 24.x` (or 25.x) and
`VAProfileH264High : VAEntrypointVLD`. Use the same clip that failed in alpha.3, if possible.

## 1. Normal run (the main question)

```sh
KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-1-default.txt
```

In the app: import the clip, put it on the timeline, play at least 5 s, stop; click somewhere
else in the timeline and play again for 5 s; drag the playhead while playing; stop; open the
Deliver page; render the timeline once (H.264, hardware encoder if offered). Quit.

Lines to look for (examples):

| Line | Answers |
|---|---|
| `kinoklip-launcher: using bundled fallbacks: ... libva.so.2 ...` (or absent) | bundled or host libva |
| `kk.media: hardware decoding VAAPI available: /dev/dri/renderD128, driver iHD 24.1.0, libva 2.22.0 (host, /usr/lib/libva.so.2.2200.0)` | driver and libva versions really used |
| `kk.media: decoding clip.mp4 (h264 high 1920x1080) with VAAPI (iHD 24.1.0, /dev/dri/renderD128)` | the decoder actually in use, per file |
| `kk.media: VAAPI decoding failed for clip.mp4 at 1.240 s (frame download: ... Failed to sync surface ...); continuing in software (4 threads)` | the failure still happens (and where) |
| `kk.media: hardware decoding VAAPI disabled for this session after 3 failures ...` | it happens repeatedly |
| `kk.export: hardware encoders: VAAPI h264/hevc (iHD ...), NVENC unavailable (no NVIDIA driver), QSV unavailable (...)` | encoders found, without error spam |
| `kk.playback: KK_PLAYBACK_STATS: one stats line per second ...` | the variable reached the app |
| `kk.playback: stats: 60.0 fps (of 60.00), 0 dropped, ... [decode 1.9, ...]` | playback speed while playing |
| `kk.playback: stats summary: played 5.02 s, 301 frames shown (60.0 fps of 60.00), 0 dropped, ...` | one per play |

There should be no `CUDA_ERROR_NO_DEVICE`, `Error creating a MFX session` or
`UDTA parsing failed` lines any more.

## 2. If "decoding failed" still appears: three short comparison runs

Same steps as run 1 each time (play 5 s twice is enough, no render needed):

```sh
# a) without the new locking: if failures become more frequent, concurrency is (part of) it
KINOKLIP_HW_SERIALIZE=off KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-2a-nolock.txt
# b) old download path (av_hwframe_transfer_data instead of map + streaming copy)
KINOKLIP_VAAPI_COPY=transfer KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-2b-transfer.txt
# c) everything in software: the reference (no hardware decoding at all)
KINOKLIP_HWACCEL=off KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-2c-software.txt
```

And one verbose run with every media detail (FFmpeg messages included):

```sh
QT_LOGGING_RULES="kk.media.debug=true" KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-3-verbose.txt
```

## 3. Optional: headless playback of a saved project

Plays the first viewer for 5 s without touching the mouse, then writes a screenshot and quits
(same playback code, handy for comparing runs exactly):

```sh
KK_PLAYBACK_STATS=1 KK_SCREENSHOT_PLAY_MS=5000 ./Kinoklip-x86_64.run --screenshot edit /tmp/kk.png ~/test.kkproj 2>&1 | tee kinoklip-4-headless.txt
```

## Environment variables (reference)

| Variable | Effect |
|---|---|
| `KK_PLAYBACK_STATS=1` | stats line per second while playing, summary on stop |
| `KINOKLIP_HWACCEL=off\|auto\|vaapi\|cuda\|vulkan` | force software or one backend for decoding |
| `KINOKLIP_HW_SERIALIZE=off\|vaapi\|all` | which hardware decoders take turns on their device (default `vaapi`) |
| `KINOKLIP_VAAPI_COPY=transfer` | VAAPI download through `av_hwframe_transfer_data` (old path) |
| `KINOKLIP_HWENC=off` | no hardware encoders for export |
| `QT_LOGGING_RULES="kk.media.debug=true"` | verbose media log (also `kk.export.debug`, `kk.playback.debug`) |
| `LIBVA_DRIVER_NAME=iHD` | (libva) force the iHD driver; leave unset normally |
