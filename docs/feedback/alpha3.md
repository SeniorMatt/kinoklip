# Owner feedback on 0.1.0-alpha.3 (T14s Gen 2, Iris Xe, KDE Linux, Plasma 6 Wayland, portable AppRun)

1. Drag and drop into the timeline does not work (media pool -> timeline; probably also files from Dolphin).
2. "How do you drag windows around, e.g. the Keyframes panel?" - detaching/moving panels is not discoverable.
3. Log of creating + rendering a small test video with `KK_PLAYBACK_STATS=1 ./AppRun`:
   - `This plugin supports grabbing the mouse only for popup windows` (x6) - Qt Wayland warning:
     something calls QWidget::grabMouse()/QWindow::setMouseGrabEnabled on a non-popup window;
     likely linked to (1).
   - `[AVHWFramesContext] Failed to sync surface 0x3: 1 (operation failed)` and
     `kk.media: hardware frame download failed: "Input/output error"` (twice) - VAAPI download failures
     on Iris Xe (iHD); suspect concurrent VA access from the new decode-ahead thread, or the new
     streaming copy path.
   - No `kk.playback: stats` lines at all (owner may not have played, or the log line is broken).
   - Noise: `UDTA parsing failed retrying raw` (FFmpeg mov demuxer), CUDA_ERROR_NO_DEVICE from CUDA
     decode and NVENC probes on a machine without NVIDIA, `h264_qsv ... Error creating a MFX session: -9`
     (QSV probes).
   - No line says which hardware decoder is actually used when it works.
4. Owner wants: fix all bugs, then move on to the next stage (v2 in docs/roadmap.md).
