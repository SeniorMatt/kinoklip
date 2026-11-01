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
#
# Format matrix (tests/media/test_formats.cpp), in <outdir>/formats/: one small file per
# import format of plan.md §5.8. Videos: testsrc2 320x240 25 fps 2 s (+ 440 Hz stereo sine);
# audio: 440 Hz sine 2 s; stills: 256x192 quadrants (top-left red, top-right lime, bottom-left
# blue, bottom-right white). A format whose encoder/tool is missing is skipped with a message
# (the test then QSKIPs it); scripts/make_test_images.py (python3) writes DNG, PSD, SVG, HEIC
# and the EXIF-rotated JPEG, which ffmpeg cannot produce.
set -euo pipefail

FIXTURE_VERSION=6
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

# 9b. HEVC Main10 (10-bit 4:2:0) 1280x720 25 fps 3 s: hardware decoders download it as P010.
"${FF[@]}" -f lavfi -i "testsrc2=s=1280x720:r=25:d=3" -f lavfi -i "$(barcode 1280 720 25 3)" \
    -filter_complex "[0][1]overlay=0:0:shortest=1,format=yuv420p10le[v]" -map "[v]" \
    -c:v libx265 -preset fast -x265-params "log-level=error:keyint=25:pools=2" -tag:v hvc1 \
    "$OUT/tmp_hevc10.mp4"
mv "$OUT/tmp_hevc10.mp4" "$OUT/hevc10.mp4"

# 10. Stills: PNG with alpha (left half opaque red, right half transparent), opaque JPEG.
"${FF[@]}" -f lavfi -i "color=c=red:s=64x48,format=rgba,geq=r=255:g=0:b=0:a='if(lt(X,W/2),255,0)'" \
    -frames:v 1 "$OUT/tmp_still_alpha.png"
mv "$OUT/tmp_still_alpha.png" "$OUT/still_alpha.png"
"${FF[@]}" -f lavfi -i "testsrc2=s=800x600" -frames:v 1 -q:v 3 "$OUT/tmp_still.jpg"
mv "$OUT/tmp_still.jpg" "$OUT/still.jpg"

# 12. Format matrix (see header). Every generator may fail without failing the script.
FMT="$OUT/formats"
mkdir -p "$FMT" "$FMT/seq_png" "$FMT/seq_dot" "$FMT/seq_unpadded"
# fmt NAME [ffmpeg args...] — writes $FMT/NAME (format from the extension, or -f in args).
fmt() {
    local name=$1
    shift
    if "${FF[@]}" "$@" "$FMT/tmp_$name" 2> "$FMT/.err"; then
        mv "$FMT/tmp_$name" "$FMT/$name"
    else
        echo "make_test_media.sh: skipping formats/$name: $(tail -n 1 "$FMT/.err")" >&2
        rm -f "$FMT/tmp_$name"
    fi
}
V=(-f lavfi -i "testsrc2=s=320x240:r=25:d=2")
A=(-f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2")
AS=(-ac 2) # stereo output
H264=(-c:v libx264 -preset veryfast -pix_fmt yuv420p -threads 2)

# 12a. Video containers / codecs.
fmt mp4_h264.mp4 "${V[@]}" "${A[@]}" "${H264[@]}" -c:a aac "${AS[@]}"
fmt mp4_hevc.mp4 "${V[@]}" "${A[@]}" -c:v libx265 -preset ultrafast -pix_fmt yuv420p \
    -x265-params log-level=error:pools=2 -tag:v hvc1 -c:a aac "${AS[@]}"
fmt mp4_av1.mp4 "${V[@]}" "${A[@]}" -c:v libsvtav1 -preset 12 -pix_fmt yuv420p -c:a aac "${AS[@]}"
fmt m4v_h264.m4v "${V[@]}" "${H264[@]}"
fmt mov_prores.mov "${V[@]}" "${A[@]}" -c:v prores_ks -profile:v 2 -pix_fmt yuv422p10le \
    -c:a pcm_s16le "${AS[@]}"
fmt mov_dnxhr.mov "${V[@]}" "${A[@]}" -c:v dnxhd -profile:v dnxhr_sq -pix_fmt yuv422p \
    -c:a pcm_s16le "${AS[@]}"
fmt mov_cineform.mov "${V[@]}" "${A[@]}" -c:v cfhd -pix_fmt yuv422p10le -c:a pcm_s16le "${AS[@]}"
fmt mkv_vp9.mkv "${V[@]}" "${A[@]}" -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 500k \
    -c:a libopus "${AS[@]}"
fmt mkv_ffv1.mkv "${V[@]}" "${A[@]}" -c:v ffv1 -c:a flac "${AS[@]}"
fmt webm_vp9.webm "${V[@]}" "${A[@]}" -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 500k \
    -c:a libopus "${AS[@]}"
fmt webm_vp8.webm "${V[@]}" "${A[@]}" -c:v libvpx -deadline realtime -cpu-used 8 -b:v 500k \
    -c:a libvorbis "${AS[@]}"
fmt webm_av1.webm "${V[@]}" "${A[@]}" -c:v libsvtav1 -preset 12 -c:a libopus "${AS[@]}"
fmt avi_mjpeg.avi "${V[@]}" "${A[@]}" -c:v mjpeg -q:v 3 -pix_fmt yuvj420p -c:a pcm_s16le "${AS[@]}"
fmt avi_mpeg4.avi "${V[@]}" "${A[@]}" -c:v mpeg4 -q:v 4 -c:a libmp3lame "${AS[@]}"
fmt avi_raw.avi "${V[@]}" -c:v rawvideo -pix_fmt yuv420p
fmt mts_h264.mts "${V[@]}" "${A[@]}" "${H264[@]}" -c:a ac3 "${AS[@]}" -f mpegts -mpegts_m2ts_mode 1
fmt mxf_mpeg2.mxf "${V[@]}" "${A[@]}" -c:v mpeg2video -pix_fmt yuv422p -c:a pcm_s16le "${AS[@]}"
fmt mxf_dnxhd.mxf "${V[@]}" "${A[@]}" -c:v dnxhd -profile:v dnxhr_sq -pix_fmt yuv422p \
    -c:a pcm_s16le "${AS[@]}"
fmt flv_h264.flv "${V[@]}" "${A[@]}" "${H264[@]}" -c:a aac "${AS[@]}"
fmt wmv_wmv2.wmv "${V[@]}" "${A[@]}" -c:v wmv2 -c:a wmav2 "${AS[@]}"
fmt 3gp_h263.3gp -f lavfi -i "testsrc2=s=352x288:r=25:d=2" \
    -f lavfi -i "sine=frequency=440:sample_rate=8000:duration=2" -c:v h263 -c:a aac -ac 1
fmt ogv_theora.ogv "${V[@]}" "${A[@]}" -c:v libtheora -q:v 6 -c:a libvorbis "${AS[@]}"
fmt mpg_mpeg2.mpg "${V[@]}" "${A[@]}" -c:v mpeg2video -c:a mp2 "${AS[@]}" -f mpeg
fmt vob_mpeg2.vob "${V[@]}" "${A[@]}" -c:v mpeg2video -c:a ac3 "${AS[@]}" -f vob
fmt dv_pal.dv -f lavfi -i "testsrc2=s=720x576:r=25:d=2" "${A[@]}" -c:v dvvideo -pix_fmt yuv420p \
    -c:a pcm_s16le "${AS[@]}"
fmt y4m_raw.y4m "${V[@]}" -pix_fmt yuv420p
fmt anim.gif -f lavfi -i "testsrc2=s=160x120:r=10:d=2"
fmt anim.webp -f lavfi -i "testsrc2=s=160x120:r=10:d=2" -c:v libwebp_anim -loop 0 -lossless 0

# 12b. Audio.
fmt audio.wav "${A[@]}" -c:a pcm_s16le "${AS[@]}"
fmt audio.aiff "${A[@]}" -c:a pcm_s16be "${AS[@]}"
fmt audio.flac "${A[@]}" -c:a flac "${AS[@]}"
fmt audio.mp3 "${A[@]}" -c:a libmp3lame -b:a 128k "${AS[@]}"
fmt audio.m4a "${A[@]}" -c:a aac "${AS[@]}"
fmt audio_alac.m4a "${A[@]}" -c:a alac "${AS[@]}"
fmt audio.aac "${A[@]}" -c:a aac "${AS[@]}"
fmt audio.ogg "${A[@]}" -c:a libvorbis "${AS[@]}"
fmt audio.opus "${A[@]}" -c:a libopus "${AS[@]}"
fmt audio.wma "${A[@]}" -c:a wmav2 "${AS[@]}"
fmt audio.ac3 "${A[@]}" -c:a ac3 "${AS[@]}"
fmt audio.eac3 "${A[@]}" -c:a eac3 "${AS[@]}"
fmt audio.dts "${A[@]}" -c:a dca -strict -2 "${AS[@]}"
fmt audio.caf "${A[@]}" -c:a alac "${AS[@]}"
fmt audio.w64 "${A[@]}" -c:a pcm_s24le "${AS[@]}"
fmt audio.mka "${A[@]}" -c:a libopus "${AS[@]}"

# 12c. Stills: the quadrant picture in every image format.
QUAD=(-f lavfi -i "color=c=red:s=128x96" -f lavfi -i "color=c=lime:s=128x96"
    -f lavfi -i "color=c=blue:s=128x96" -f lavfi -i "color=c=white:s=128x96"
    -filter_complex "[0][1]hstack[t];[2][3]hstack[b];[t][b]vstack" -frames:v 1)
fmt still.png "${QUAD[@]}"
if [[ -f "$FMT/still.png" ]]; then
    Q=(-i "$FMT/still.png" -frames:v 1)
    fmt still.jpg "${Q[@]}" -q:v 2
    fmt still.webp "${Q[@]}" -c:v libwebp -quality 95
    fmt still.bmp "${Q[@]}"
    fmt still.gif "${Q[@]}"
    fmt still.tiff "${Q[@]}" -pix_fmt rgb24
    fmt still.tga "${Q[@]}"
    fmt still.ico "${Q[@]}"
    fmt still.avif "${Q[@]}" -c:v libaom-av1 -still-picture 1 -cpu-used 8 -crf 10 -pix_fmt yuv444p
    fmt still.jxl "${Q[@]}" -c:v libjxl
    fmt still.exr "${Q[@]}" -pix_fmt gbrpf32le
    fmt still.dpx "${Q[@]}" -pix_fmt rgb24
    fmt still.qoi "${Q[@]}"
    fmt still.pcx "${Q[@]}" -pix_fmt rgb24
    fmt still.sgi "${Q[@]}"
    fmt still.j2k "${Q[@]}" -c:v jpeg2000 -pix_fmt rgb24
fi
if command -v python3 > /dev/null; then
    python3 "$ROOT/scripts/make_test_images.py" "$FMT" dng psd svg heic exifjpeg
else
    echo "make_test_media.sh: python3 not found; skipping DNG/PSD/SVG/HEIC/EXIF fixtures" >&2
fi

# 12d. Image sequences (frame-index barcode, see the header; file number n shows index n - start).
SEQ=(-f lavfi -i "testsrc2=s=160x120:r=25:d=0.4" -f lavfi -i "$(barcode 160 120 25 0.4)"
    -filter_complex "[0][1]overlay=0:0:shortest=1")
"${FF[@]}" "${SEQ[@]}" -start_number 1 "$FMT/seq_png/frame_%04d.png"
rm -f "$FMT/seq_png/frame_0005.png" # a gap: number 5 is missing (frames 1..10 otherwise)
"${FF[@]}" "${SEQ[@]}" -start_number 100 -frames:v 5 -q:v 2 "$FMT/seq_dot/shot.%04d.jpg"
"${FF[@]}" "${SEQ[@]}" -start_number 1 -frames:v 10 "$FMT/seq_unpadded/%d.png"
rm -f "$FMT/.err"

# 11. Broken files: random bytes with a video extension, and a truncated MP4 (header intact,
#     media data cut off at 40 %).
head -c 65536 /dev/urandom > "$OUT/corrupt.mp4"
size=$(stat -c %s "$OUT/h264_25fps.mp4")
head -c $((size * 4 / 10)) "$OUT/h264_25fps.mp4" > "$OUT/truncated.mp4"
: > "$OUT/empty.mov"

rm -f "$OUT"/.complete-v*
touch "$STAMP"
echo "Done."
