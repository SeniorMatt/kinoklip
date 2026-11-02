#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Generates the larger playback-performance clips (not used by ctest's default runs):
#   screen_1080p60.mp4  1920x1080 60 fps H.264, 20 s: a screen recording (static window with
#                       sharp small text, a moving cursor box, a scrolling text panel)
#   screen_1440p60.mp4  the same at 2560x1440
#   hevc_2160p30.mp4    3840x2160 30 fps HEVC 8-bit, 10 s (testsrc2, full motion)
# Usage: scripts/make_perf_media.sh [out-dir]   (default tests/data/generated/perf)
# Play them with: KK_PERF_MEDIA=$(ls -d tests/data/generated/perf/*.mp4 | paste -sd:) \
#                 build/test_playbackperf singleClipPlayback
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/tests/data/generated/perf}"
mkdir -p "$OUT"
FF=(ffmpeg -hide_banner -loglevel error -nostdin -y)

screen() { # W H NAME
    local w=$1 h=$2 name=$3 fs=$(($2 / 60))
    [[ -f "$OUT/$name" ]] && return
    # Background "desktop", a light window with many lines of small text, a scrolling log
    # panel and a cursor-sized box moving across it: mostly static, sharp edges.
    "${FF[@]}" -f lavfi -i "color=c=0x2a5d8f:s=${w}x${h}:r=60:d=20" -vf \
"drawbox=x=$((w / 12)):y=$((h / 12)):w=$((w * 5 / 6)):h=$((h * 5 / 6)):color=0xf0f0f0:t=fill,\
drawbox=x=$((w / 12)):y=$((h / 12)):w=$((w * 5 / 6)):h=$((h / 24)):color=0xd0d0d8:t=fill,\
drawtext=fontsize=$fs:fontcolor=0x202020:x=$((w / 10)):y=$((h / 6)):line_spacing=$((fs / 2)):text='int main(int argc, char** argv) { return QApplication(argc, argv).exec(); }
  // The quick brown fox jumps over the lazy dog 0123456789 !@#\$%^&*()
  void PlaybackEngine::onTick() { const int64_t now = monotonicUs(); }
  kk\:\:Compositor compositor; compositor.renderAsync(request);
  Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor.',\
drawtext=fontsize=$((fs * 3 / 4)):fontcolor=0x106010:x=$((w / 10)):y=$((h / 2))-mod(n*2\,$((h / 4))):text='%{pts\:hms} frame %{n} build log line ok',\
drawbox=x=mod(t*420\,$((w - 40))):y=$((h * 2 / 3))+mod(t*180\,$((h / 6))):w=$((h / 60)):h=$((h / 40)):color=black:t=fill" \
        -c:v libx264 -preset veryfast -crf 18 -g 120 -pix_fmt yuv420p -threads 4 \
        "$OUT/tmp_$name"
    mv "$OUT/tmp_$name" "$OUT/$name"
}

screen 1920 1080 screen_1080p60.mp4
screen 2560 1440 screen_1440p60.mp4
if [[ ! -f "$OUT/hevc_2160p30.mp4" ]]; then
    "${FF[@]}" -f lavfi -i "testsrc2=s=3840x2160:r=30:d=10" -pix_fmt yuv420p -c:v libx265 \
        -preset ultrafast -x265-params "log-level=error:keyint=60:pools=4" -tag:v hvc1 \
        "$OUT/tmp_hevc_2160p30.mp4"
    mv "$OUT/tmp_hevc_2160p30.mp4" "$OUT/hevc_2160p30.mp4"
fi
