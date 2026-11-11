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
  only after one full second of uninterrupted playback, so short plays printed nothing);
* FFmpeg's messages stay in Kinoklip's log routing: Qt Multimedia's FFmpeg backend replaced
  FFmpeg's log callback as soon as audio started, which is why alpha.3 showed raw
  `[AVHWFramesContext] ...`, `UDTA ...`, `CUDA_ERROR_NO_DEVICE` and MFX lines.

`QT_FORCE_STDERR_LOGGING=1` is added to every command below: the bundled Qt sends its log to
the systemd journal instead of the terminal when it thinks there is no console (e.g. when the
`.run` is started from Dolphin); with the variable the lines always reach `tee`. (Already
started without it? `journalctl --user -b | grep kk\.` finds them.)

Since 1.2 the lines this test asks for (decoder per file, driver, playback statistics, warnings)
are also kept in memory without any variable: **Help > Export Log File...** saves them as one
text file (see [Send a log file with a bug report](guide/send-a-log-file-with-a-bug-report.md)).
The steps below still work for a run from a terminal.

Please run the steps below and send `kinoklip-*.txt` back. Each run takes about two minutes.

## 4K HEVC is choppy (Core Ultra 7 258V / Arc 140V report, 1.4)

Since 1.4 a VAAPI decoder hands its frames to the GPU as DMA-BUF handles instead of copying every
4K picture through system memory (spec 77.2), and the log says in plain words what is used and why.
If 4K HEVC is still choppy, run this and send the file from **Help > Export Log File...** (or the
`tee` output):

```sh
vainfo --display drm --device /dev/dri/renderD128 2>&1 | head -30     # driver version, VAProfileHEVCMain/Main10 : VAEntrypointVLD
ls -l /dev/dri; id                                                      # you need the render group (or a logged-in seat) for renderD128
QT_FORCE_STDERR_LOGGING=1 KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.AppImage 2>&1 | tee kinoklip-4k.txt   # play the 4K clip for 10 s, stop, quit
KINOKLIP_ZEROCOPY=0 QT_FORCE_STDERR_LOGGING=1 KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.AppImage 2>&1 | tee kinoklip-4k-download.txt   # same, with the old download path
KINOKLIP_HWACCEL=off QT_FORCE_STDERR_LOGGING=1 KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.AppImage 2>&1 | tee kinoklip-4k-software.txt    # software reference
```

On an X11 session add `KINOKLIP_X11_EGL=1` to the first command (zero-copy needs an EGL context;
Wayland has one, X11 uses GLX unless asked).

Read the log from the top:

| Line | Meaning |
|---|---|
| `hardware decoding VAAPI available: /dev/dri/renderD128, driver iHD 24.x, libva 2.x (host\|bundled, ...), decodes: ... HEVC Main yes, HEVC Main10 yes ...` | VAAPI works; the driver and libva in use; what the GPU decodes |
| `hardware decoding VAAPI unavailable: ... [GPU PCI id 8086:6420; libva ...; drivers: ...]` | VAAPI does not start; the text after it says which GPU, which libva, which driver files libva can see (a driver too old for the GPU shows here as "unknown libva error") |
| `decoding clip.mp4 (hevc main 10 3840x2160) with VAAPI (iHD ..., /dev/dri/renderD128), output: DMA-BUF (zero-copy, no download)` | the good case |
| `... with software (6 threads); VAAPI failed: ... (<FFmpeg's reason>)` | the GPU or driver refused this stream; the reason is FFmpeg's own message |
| `zero-copy decoding (VAAPI -> EGL DMA-BUF): ready ...` / `unavailable: <why>` | the GL side; `unavailable` names the missing piece (EGL context, extension) |
| `zero-copy decoding: the first DMA-BUF frame was imported by the GPU` | proof that the path works on this machine |
| `kk.playback: stats: ... [decode ..., upload ..., composite ..., output ...], ..., frames: 59 zero-copy (0.1 ms each), 0 downloaded (0.0 ms each), 0 software` | per second: how frames reached the GPU and what each cost |
| `playback sources: the timeline plays proxy or optimized media ...` / `... the original files` | whether proxies are used while playing |

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
QT_FORCE_STDERR_LOGGING=1 KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-1-default.txt
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
KINOKLIP_HW_SERIALIZE=off QT_FORCE_STDERR_LOGGING=1 KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-2a-nolock.txt
# b) old download path (av_hwframe_transfer_data instead of map + streaming copy)
KINOKLIP_VAAPI_COPY=transfer QT_FORCE_STDERR_LOGGING=1 KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-2b-transfer.txt
# c) everything in software: the reference (no hardware decoding at all)
KINOKLIP_HWACCEL=off QT_FORCE_STDERR_LOGGING=1 KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-2c-software.txt
```

And one verbose run with every media detail (FFmpeg messages included):

```sh
QT_LOGGING_RULES="kk.media.debug=true" QT_FORCE_STDERR_LOGGING=1 KK_PLAYBACK_STATS=1 ./Kinoklip-x86_64.run 2>&1 | tee kinoklip-3-verbose.txt
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
| `KINOKLIP_ZEROCOPY=0` | no zero-copy: VAAPI frames are downloaded to system memory (the 1.3 path) |
| `KINOKLIP_X11_EGL=1` | X11: ask Qt for an EGL context (needed for zero-copy; Wayland always has one) |
| `KINOKLIP_SW_THREADS=<n>` | software decoding threads (default: 4, 3/4 of the cores above 1080p) |
| `KINOKLIP_VAAPI_COPY=transfer` | VAAPI download through `av_hwframe_transfer_data` (old path) |
| `KINOKLIP_HWENC=off` | no hardware encoders for export |
| `QT_LOGGING_RULES="kk.media.debug=true"` | verbose media log (also `kk.export.debug`, `kk.playback.debug`) |
| `QT_FORCE_STDERR_LOGGING=1` | (Qt) always log to stderr, never to the journal |
| `LIBVA_DRIVER_NAME=iHD` | (libva) force the iHD driver; leave unset normally |
