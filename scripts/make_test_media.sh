#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# make_test_media.sh — generates the small media fixtures used by tests/media (needs `ffmpeg`).
#
# Usage: scripts/make_test_media.sh [outdir]      (default: tests/data/generated, gitignored)
#
# Idempotent: exits immediately when the fixtures of the current FIXTURE_VERSION exist.
# Safe to run concurrently (tests may start in parallel): a lock file serialises runs.
#
# Seek-accuracy fixtures carry a BARCODE in the top 1/8 of every frame: 16 equal columns,
# two rows. Row 0, columns 0..11 = bits of the frame index N (LSB first, white = 1);
# columns 12..15 = fixed marker 1,0,1,0. Row 1 = row 0 inverted. Blocks are large and pure
# black/white, so they survive H.264 compression and scaling; tests decode the index and know
# exactly which frame the decoder returned (see tests/media/MediaTestUtil.h).
#
# Audio sample-accuracy fixtures:
#   mono_ramp_48k.wav  — sawtooth: sample n has value ((n mod 4800) / 4800) * 2 - 1, so every
#                        decoded sample reveals its own position.
#   impulses_44k.wav   — 44.1 kHz, a single full-scale sample at 22050*k + 1000 (k = 0..5);
#                        checks position accuracy through the 44.1 → 48 kHz resampler.
set -euo pipefail

FIXTURE_VERSION=4
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/tests/data/generated}"
STAMP="$OUT/.complete-v$FIXTURE_VERSION"

if [[ -f "$STAMP" ]]; then
    exit 0
fi
if ! command -v ffmpeg > /dev/null; then
    echo "make_test_media.sh: ffmpeg not found in PATH" >&2
    exit 2
fi
mkdir -p "$OUT"

# Serialise concurrent runs; the second runner waits and then sees the stamp.
if command -v flock > /dev/null; then
    exec 9> "$OUT/.lock"
    flock 9
    [[ -f "$STAMP" ]] && exit 0
fi

# Keep encoder thread use modest (shared build machines).
FF=(ffmpeg -hide_banner -loglevel error -nostdin -y)
X264=(-c:v libx264 -preset veryfast -pix_fmt yuv420p -threads 2)

# barcode W H RATE DURATION → lavfi source drawing the frame-index barcode strip (W × H/8).
barcode() {
    local w=$1 h=$2 rate=$3 dur=$4
    local bit="if(lt(X,12),mod(floor(N/pow(2,X)),2),eq(mod(X,2),0))"
    echo "color=c=black:s=16x2:r=$rate:d=$dur,format=gray,geq=lum='255*if(eq(Y,0),$bit,1-$bit)',scale=$w:$((h / 8)):flags=neighbor"
}

# barcoded_video W H RATE DURATION OUT [extra ffmpeg args...] — testsrc2 + barcode overlay.
barcoded_video() {
    local w=$1 h=$2 rate=$3 dur=$4 out=$5
    shift 5
    "${FF[@]}" -f lavfi -i "testsrc2=s=${w}x${h}:r=$rate:d=$dur" \
        -f lavfi -i "$(barcode "$w" "$h" "$rate" "$dur")" \
        "$@" -filter_complex "[0][1]overlay=0:0:shortest=1,format=yuv420p[v]" -map "[v]" \
        "${X264[@]}" "$OUT/tmp_$out"
    mv "$OUT/tmp_$out" "$OUT/$out"
}

echo "Generating test media in $OUT ..."

# 1. H.264 25 fps 10 s 1280x720 (2 s GOP, B-frames) + AAC 48 kHz stereo sine.
barcoded_video 1280 720 25 10 h264_25fps.mp4 \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" \
    -map 2:a -c:a aac -b:a 128k -ac 2 -g 50 -movflags +faststart

# 2. H.264 29.97 fps (30000/1001) 6 s 640x360 + AAC 44.1 kHz mono (exercises resampling).
barcoded_video 640 360 30000/1001 6 h264_2997.mp4 \
    -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=6" \
    -map 2:a -c:a aac -b:a 96k -ac 1 -g 30

# 3. H.264 60 fps 4 s 640x360, video only, long GOP (4 s).
barcoded_video 640 360 60 4 h264_60fps.mp4 -g 240

# 4. MPEG-TS 25 fps 4 s: non-zero start time and no up-front seek index.
barcoded_video 640 360 25 4 h264_25fps.ts -g 25

# 5. Variable frame rate, odd size (321x241, 4:4:4): frames 0..29 at 1/30 s, 30..59 at 1/15 s.
#    (Integer pts in a 1/30 time base: float expressions round some frames to the wrong tick.)
"${FF[@]}" -f lavfi -i "testsrc2=s=320x240:r=30:d=2" -f lavfi -i "$(barcode 321 241 30 2)" \
    -filter_complex "[0]scale=321:241[bg];[bg][1]overlay=0:0:shortest=1,settb=1/30,setpts='if(lt(N,30),N,30+2*(N-30))',format=yuv444p[v]" \
    -map "[v]" -fps_mode vfr -c:v libx264 -preset veryfast -pix_fmt yuv444p -threads 2 \
    "$OUT/tmp_vfr_odd.mp4"
mv "$OUT/tmp_vfr_odd.mp4" "$OUT/vfr_odd.mp4"

# 6. ProRes 4444 with alpha: left half opaque, right half fully transparent.
"${FF[@]}" -f lavfi -i "testsrc2=s=320x240:r=25:d=2,format=rgba,geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='if(lt(X,W/2),255,0)'" \
    -c:v prores_ks -profile:v 4444 -pix_fmt yuva444p10le -threads 2 "$OUT/tmp_prores4444_alpha.mov"
mv "$OUT/tmp_prores4444_alpha.mov" "$OUT/prores4444_alpha.mov"

# 7. Phone-style portrait clip: coded 640x360 landscape + "rotate 90° clockwise" display
#    matrix. A red box marks the coded top-left corner (displayed at the top-right).
"${FF[@]}" -f lavfi -i "color=c=blue:s=640x360:r=25:d=2,drawbox=x=0:y=0:w=160:h=90:c=red:t=fill" \
    "${X264[@]}" "$OUT/tmp_rotated_base.mp4"
"${FF[@]}" -display_rotation -90 -i "$OUT/tmp_rotated_base.mp4" -c copy "$OUT/tmp_rotated.mp4"
rm -f "$OUT/tmp_rotated_base.mp4"
mv "$OUT/tmp_rotated.mp4" "$OUT/rotated_portrait.mp4"

# 8. Audio-only fixtures.
"${FF[@]}" -f lavfi -i "aevalsrc='mod(n,4800)/4800*2-1':s=48000:d=5" -c:a pcm_s16le -ac 1 \
    "$OUT/tmp_mono_ramp_48k.wav"
mv "$OUT/tmp_mono_ramp_48k.wav" "$OUT/mono_ramp_48k.wav"
"${FF[@]}" -f lavfi -i "aevalsrc='if(eq(mod(n,22050),1000),1,0)':s=44100:d=3" -c:a pcm_s16le \
    -ac 1 "$OUT/tmp_impulses_44k.wav"
mv "$OUT/tmp_impulses_44k.wav" "$OUT/impulses_44k.wav"
"${FF[@]}" -f lavfi -i "sine=frequency=660:sample_rate=44100:duration=3" -ac 2 \
    -c:a libmp3lame -b:a 128k "$OUT/tmp_sine.mp3"
mv "$OUT/tmp_sine.mp3" "$OUT/sine.mp3"

# 9. 1080p H.264 for performance measurements (8 s, 2 s GOP).
barcoded_video 1920 1080 25 8 perf_1080p.mp4 -g 50

# 10. Stills: PNG with alpha (left half opaque red, right half transparent), opaque JPEG.
"${FF[@]}" -f lavfi -i "color=c=red:s=64x48,format=rgba,geq=r=255:g=0:b=0:a='if(lt(X,W/2),255,0)'" \
    -frames:v 1 "$OUT/tmp_still_alpha.png"
mv "$OUT/tmp_still_alpha.png" "$OUT/still_alpha.png"
"${FF[@]}" -f lavfi -i "testsrc2=s=800x600" -frames:v 1 -q:v 3 "$OUT/tmp_still.jpg"
mv "$OUT/tmp_still.jpg" "$OUT/still.jpg"

# 11. Broken files: random bytes with a video extension, and a truncated MP4 (header intact,
#     media data cut off at 40 %).
head -c 65536 /dev/urandom > "$OUT/corrupt.mp4"
size=$(stat -c %s "$OUT/h264_25fps.mp4")
head -c $((size * 4 / 10)) "$OUT/h264_25fps.mp4" > "$OUT/truncated.mp4"
: > "$OUT/empty.mov"

rm -f "$OUT"/.complete-v*
touch "$STAMP"
echo "Done."
