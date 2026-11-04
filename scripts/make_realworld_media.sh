#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# make_realworld_media.sh — the "real-world media" fixtures of tests/media/test_realworld.cpp
# (bug hunt round 11): the files beginners throw at an editor. Called by make_test_media.sh
# (which passes its output directory); writes <outdir>/realworld/.
#
# Usage: scripts/make_realworld_media.sh [outdir]   (default: tests/data/generated)
#
# Every file is tiny (a few seconds, small sizes; the 4K sample is 1 s of flat colour).
# Idempotent and safe to run concurrently (own version stamp and lock, independent of the
# main fixtures). A file whose encoder is missing is skipped with a message (the test QSKIPs).
#
# Quadrant pictures (phone_rot*.mp4, big_photo_exif6.jpg) are coded TL red, TR lime,
# BL blue, BR white; tests check the corners to see whether the rotation was applied.
# phone_hlg.mp4 / phone_pq.mp4 carry known HDR code values (BT.2020, HLG / PQ):
#   HLG left half: grey at 75 % signal (HLG reference white, 203 cd/m2); right half: the
#   BT.709 primary green expressed in BT.2020 HLG. PQ: grey at the 203 cd/m2 code value.
set -euo pipefail

RW_VERSION=1
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE="${1:-$ROOT/tests/data/generated}"
RW="$BASE/realworld"
STAMP="$RW/.complete-v$RW_VERSION"

[[ -f "$STAMP" ]] && exit 0
if ! command -v ffmpeg > /dev/null; then
    echo "make_realworld_media.sh: ffmpeg not found in PATH" >&2
    exit 2
fi
mkdir -p "$RW"
if command -v flock > /dev/null; then
    exec 8> "$RW/.lock"
    flock 8
    [[ -f "$STAMP" ]] && exit 0
fi

FF=(ffmpeg -hide_banner -loglevel error -nostdin -y)
X264=(-c:v libx264 -preset veryfast -pix_fmt yuv420p -threads 2)
X265=(-c:v libx265 -preset ultrafast -tag:v hvc1)

# rw NAME [ffmpeg args...] — writes $RW/NAME; a failing encoder only skips that file.
rw() {
    local name=$1
    shift
    if "${FF[@]}" "$@" "$RW/tmp_$name" 2> "$RW/.err"; then
        mv "$RW/tmp_$name" "$RW/$name"
    else
        echo "make_realworld_media.sh: skipping realworld/$name: $(tail -n 1 "$RW/.err")" >&2
        rm -f "$RW/tmp_$name"
    fi
}

# quad W H RATE DURATION — lavfi graph of the quadrant picture.
quad() {
    local w=$(($1 / 2)) h=$(($2 / 2)) r=$3 d=$4
    local c="s=${w}x${h}:r=$r:d=$d"
    echo "color=c=red:$c[a];color=c=lime:$c[b];color=c=blue:$c[c];color=c=white:$c[d];[a][b]hstack[t];[c][d]hstack[u];[t][u]vstack"
}
S48=(-f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2")

echo "Generating real-world test media in $RW ..."

# 1. Phone footage.
# Portrait: coded 640x360 landscape quadrants + display matrix (90 / 270 / 180 clockwise),
# AAC stereo, moov at the end (as phones write it).
rw tmp_quad.mp4 -f lavfi -i "$(quad 640 360 30 2)" "${S48[@]}" "${X264[@]}" -c:a aac -ac 2 \
    -shortest
if [[ -f "$RW/tmp_quad.mp4" ]]; then
    for r in 90 270 180; do
        rw "phone_rot$r.mp4" -display_rotation $(((360 - r) % 360)) -i "$RW/tmp_quad.mp4" -c copy
    done
    rm -f "$RW/tmp_quad.mp4"
fi
# Variable frame rate: 90 kHz time base, ~30 fps with uneven frame durations, AAC audio.
rw phone_vfr.mp4 -f lavfi -i "testsrc2=s=640x360:r=30:d=3" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=3" \
    -filter_complex "[0]settb=1/90000,setpts='N*3000+if(gte(N,45),floor((N-45)/3)*750,0)'[v]" \
    -map "[v]" -map 1:a -fps_mode vfr -video_track_timescale 90000 "${X264[@]}" -c:a aac -ac 2
# HDR: HEVC Main10, BT.2020, HLG and PQ (known code values, see the header).
rw phone_hlg.mp4 -f lavfi -i "color=c=0xBFBFBF:s=320x180:r=25:d=2,format=rgb24,drawbox=x=160:y=0:w=160:h=180:color=0x82BB43:t=fill,scale=out_color_matrix=bt2020:out_range=tv,format=yuv420p10le" \
    "${X265[@]}" -x265-params "log-level=error:pools=2:colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc" \
    -color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc
rw phone_pq.mp4 -f lavfi -i "color=c=0x949494:s=320x180:r=25:d=2,format=rgb24,scale=out_color_matrix=bt2020:out_range=tv,format=yuv420p10le" \
    "${X265[@]}" -x265-params "log-level=error:pools=2:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc" \
    -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc
# 4K at 60 fps, 1 s (flat quadrants keep it small).
rw uhd60.mp4 -f lavfi -i "$(quad 3840 2160 60 1)" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" \
    -c:v libx264 -preset ultrafast -pix_fmt yuv420p -threads 2 -c:a aac

# 2. Camera and pro formats (ProRes 4444 alpha, DNxHR SQ, AVI MJPEG, MXF are in formats/).
rw cam_prores422hq.mov -f lavfi -i "testsrc2=s=320x240:r=25:d=2" "${S48[@]}" \
    -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le -c:a pcm_s24le
rw cam_mjpeg.mov -f lavfi -i "testsrc2=s=320x240:r=30:d=2" "${S48[@]}" \
    -c:v mjpeg -q:v 3 -pix_fmt yuvj422p -c:a pcm_s16le
rw cam_dnxhr_hq.mxf -f lavfi -i "testsrc2=s=640x360:r=25:d=1" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" \
    -c:v dnxhd -profile:v dnxhr_hq -pix_fmt yuv422p -c:a pcm_s16le

# 3. Screen recordings: laptop and 1440p sizes, an odd size (4:4:4, as 4:2:0 needs even
# sizes), 5 fps, and one GOP for the whole 12 s.
rw scr_1366x768.mp4 -f lavfi -i "testsrc2=s=1366x768:r=30:d=1" "${X264[@]}"
rw scr_1440p.mp4 -f lavfi -i "testsrc2=s=2560x1440:r=30:d=0.5" "${X264[@]}"
rw scr_odd_1281x721.mp4 -f lavfi -i "testsrc2=s=1280x720:r=30:d=1,scale=1281:721,setsar=1" \
    -c:v libx264 -preset veryfast -pix_fmt yuv444p -threads 2
rw scr_5fps.mp4 -f lavfi -i "testsrc2=s=640x360:r=5:d=4" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=4" "${X264[@]}" -c:a aac
rw scr_longgop.mp4 -f lavfi -i "testsrc2=s=320x240:r=30:d=12" "${X264[@]}" \
    -g 9999 -keyint_min 9999 -sc_threshold 0

# 4. Audio.
SPLIT6="asplit=6[c1][c2][c3][c4][c5][c6];[c1][c2][c3][c4][c5][c6]join=inputs=6:channel_layout=5.1"
rw aud_51.wav -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
    -filter_complex "[0]$SPLIT6" -c:a pcm_s16le
rw aud_51.mp4 -f lavfi -i "testsrc2=s=320x240:r=25:d=2" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
    -filter_complex "[1]$SPLIT6[o]" -map 0:v -map "[o]" "${X264[@]}" -c:a aac
rw aud_24bit.wav -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" -ac 2 \
    -c:a pcm_s24le
rw aud_44k_stereo.wav -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" -ac 2 \
    -c:a pcm_s16le
rw aud_mono_44k.m4a -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" -ac 1 \
    -c:a aac
rw tmp_cover.jpg -f lavfi -i "color=c=orange:s=300x300" -frames:v 1
if [[ -f "$RW/tmp_cover.jpg" ]]; then
    rw aud_cover.mp3 -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
        -i "$RW/tmp_cover.jpg" -map 0:a -map 1:v -c:a libmp3lame -b:a 128k -c:v copy \
        -id3v2_version 3 -disposition:v attached_pic -metadata:s:v title="Album cover"
    rw aud_cover.m4a -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
        -i "$RW/tmp_cover.jpg" -map 0:a -map 1:v -c:a aac -c:v copy -disposition:v attached_pic
    rm -f "$RW/tmp_cover.jpg"
fi
rw two_audio.mp4 -f lavfi -i "testsrc2=s=320x240:r=25:d=2" "${S48[@]}" \
    -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=2" \
    -map 0:v -map 1:a -map 2:a "${X264[@]}" -c:a aac -ac 2

# 5. Pictures: a 12-megapixel phone photo (4032x3024 coded) with EXIF orientation 6.
rw tmp_big_photo.jpg -f lavfi -i "$(quad 4032 3024 1 1)" -frames:v 1 -q:v 4
if [[ -f "$RW/tmp_big_photo.jpg" ]]; then
    python3 - "$RW/tmp_big_photo.jpg" "$RW/big_photo_exif6.jpg" << 'PY'
import os, struct, sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
# Minimal little-endian TIFF with one IFD entry: Orientation (0x0112) = 6.
tiff = b"II*\x00" + struct.pack("<I", 8) + struct.pack("<H", 1)
tiff += struct.pack("<HHII", 0x0112, 3, 1, 6) + struct.pack("<I", 0)
payload = b"Exif\x00\x00" + tiff
app1 = b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload
open(dst + ".tmp", "wb").write(data[:2] + app1 + data[2:])
os.replace(dst + ".tmp", dst)
os.remove(src)
PY
fi

# 6. Broken and tricky files (corrupt.mp4, truncated.mp4 and empty.mov are in the main set).
# A recording cut off before the moov atom was written (moov at the end, file cut in half).
rw tmp_moovend.mp4 -f lavfi -i "testsrc2=s=320x240:r=25:d=4" "${S48[@]}" "${X264[@]}" -c:a aac
if [[ -f "$RW/tmp_moovend.mp4" ]]; then
    size=$(stat -c %s "$RW/tmp_moovend.mp4")
    head -c $((size / 2)) "$RW/tmp_moovend.mp4" > "$RW/cut_no_moov.mp4"
    rm -f "$RW/tmp_moovend.mp4"
fi
# Wrong extensions: MP4 named .mov, WAV named .mp3, PNG named .jpg, text named .mp4.
rw mp4_named.mov -f lavfi -i "testsrc2=s=320x240:r=25:d=2" "${S48[@]}" "${X264[@]}" -c:a aac \
    -f mp4
rw wav_named.mp3 "${S48[@]}" -c:a pcm_s16le -f wav
rw png_named.jpg -f lavfi -i "color=c=red:s=64x48" -frames:v 1 -c:v png -f image2
printf 'This is not a video, just a text file.\n' > "$RW/text_named.mp4"

rm -f "$RW/.err" "$RW"/.complete-v*
touch "$STAMP"
echo "Done."
