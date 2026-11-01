#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# make_test_images.py — writes still-image fixtures that the ffmpeg CLI cannot produce:
# a camera-RAW DNG, a layered-format PSD, an SVG, a HEIC (built around an x265 bitstream made
# by ffmpeg), and a JPEG with an EXIF orientation tag. Pure standard library (Pillow is used
# for the EXIF JPEG only when installed). Called by make_test_media.sh; each generator is
# independent, failures are reported and skipped (the tests then QSKIP that format).
#
# Usage: make_test_images.py <outdir> <kind>...     kinds: dng psd svg heic exifjpeg
#
# Common test picture (W x H, "coded" orientation): four quadrants
#   top-left red, top-right green, bottom-left blue, bottom-right white.
# Tests check the quadrant colours, which also reveals whether an orientation was applied.
import os
import shutil
import struct
import subprocess
import sys
import tempfile

W, H = 256, 192

QUADRANTS = {  # (left?, top?) -> 8-bit sRGB
    (True, True): (255, 0, 0),
    (False, True): (0, 255, 0),
    (True, False): (0, 0, 255),
    (False, False): (255, 255, 255),
}


def colour_at(x, y, w=W, h=H):
    return QUADRANTS[(x < w // 2, y < h // 2)]


def write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


# ---- DNG (camera RAW): 16-bit RGGB Bayer mosaic, uncompressed, EXIF orientation 6 ----------
#
# A minimal DNG is a little-endian TIFF whose IFD0 holds the CFA image plus the DNG colour
# tags. ColorMatrix1 is the XYZ -> linear sRGB matrix, so "camera RGB" == linear sRGB and the
# developed picture shows the intended colours. Orientation 6 = "rotate 90 deg clockwise to
# display", so a correct loader returns a portrait image with red at the top-RIGHT.
def make_dng(out):
    white = 4095
    black = 0
    # 2x2 RGGB pattern: (0,0)=R (0,1)=G (1,0)=G (1,1)=B.
    rows = bytearray()
    lin = {255: 0.8, 0: 0.02}  # linear level of an 8-bit channel value (bright, not clipped)
    for y in range(H):
        for x in range(W):
            r, g, b = colour_at(x, y)
            site = (y % 2) * 2 + (x % 2)
            channel = r if site == 0 else (b if site == 3 else g)
            rows += struct.pack("<H", int(lin[channel] * white))
    image = bytes(rows)

    entries = []  # (tag, type, count, value-bytes)
    SHORT, LONG, RATIONAL, SRATIONAL, BYTE, ASCII = 3, 4, 5, 10, 1, 2

    def add(tag, typ, values):
        if typ == ASCII:
            data = values.encode() + b"\0"
            entries.append((tag, typ, len(data), data))
            return
        fmt = {SHORT: "H", LONG: "I", BYTE: "B"}.get(typ)
        if fmt:
            data = struct.pack("<" + fmt * len(values), *values)
        else:  # (S)RATIONAL: values are (num, den) pairs
            data = b"".join(struct.pack("<ii" if typ == SRATIONAL else "<II", n, d) for n, d in values)
        entries.append((tag, typ, len(values), data))

    xyz_to_srgb = [3.2406, -1.5372, -0.4986, -0.9689, 1.8758, 0.0415, 0.0557, -0.2040, 1.0570]
    add(254, LONG, [0])  # NewSubfileType: main image
    add(256, LONG, [W])
    add(257, LONG, [H])
    add(258, SHORT, [16])  # BitsPerSample
    add(259, SHORT, [1])  # Compression: none
    add(262, SHORT, [32803])  # PhotometricInterpretation: CFA
    add(271, ASCII, "Kinoklip")  # Make
    add(272, ASCII, "Test Camera")  # Model
    add(273, LONG, [0])  # StripOffsets (patched below)
    add(274, SHORT, [6])  # Orientation: rotate 90 CW
    add(277, SHORT, [1])  # SamplesPerPixel
    add(278, LONG, [H])  # RowsPerStrip
    add(279, LONG, [len(image)])  # StripByteCounts
    add(284, SHORT, [1])  # PlanarConfiguration
    add(33421, SHORT, [2, 2])  # CFARepeatPatternDim
    add(33422, BYTE, [0, 1, 1, 2])  # CFAPattern: RGGB
    add(50706, BYTE, [1, 4, 0, 0])  # DNGVersion 1.4
    add(50707, BYTE, [1, 1, 0, 0])  # DNGBackwardVersion
    add(50708, ASCII, "Kinoklip Test Camera")  # UniqueCameraModel
    add(50714, LONG, [black])  # BlackLevel
    add(50717, LONG, [white])  # WhiteLevel
    add(50721, SRATIONAL, [(int(v * 10000), 10000) for v in xyz_to_srgb])  # ColorMatrix1
    add(50728, RATIONAL, [(1, 1), (1, 1), (1, 1)])  # AsShotNeutral
    add(50778, SHORT, [21])  # CalibrationIlluminant1: D65
    entries.sort(key=lambda e: e[0])

    header = struct.pack("<2sHI", b"II", 42, 8)
    ifd_size = 2 + 12 * len(entries) + 4
    extra_offset = 8 + ifd_size
    extra = bytearray()
    blobs = []
    for tag, typ, count, data in entries:
        if len(data) <= 4:
            blobs.append(data.ljust(4, b"\0"))
        else:
            blobs.append(struct.pack("<I", extra_offset + len(extra)))
            extra += data
            if len(extra) % 2:
                extra += b"\0"
    strip_offset = extra_offset + len(extra)
    ifd = bytearray(struct.pack("<H", len(entries)))
    for (tag, typ, count, data), blob in zip(entries, blobs):
        if tag == 273:
            blob = struct.pack("<I", strip_offset)
        ifd += struct.pack("<HHI", tag, typ, count) + blob
    ifd += struct.pack("<I", 0)
    write_atomic(os.path.join(out, "raw_rggb.dng"), header + bytes(ifd) + bytes(extra) + image)


# ---- PSD: 8-bit RGB, three channels, raw (uncompressed) composite image ----------------------
def make_psd(out):
    data = bytearray()
    data += struct.pack(">4sH6sHIIHH", b"8BPS", 1, b"\0" * 6, 3, H, W, 8, 3)  # header, RGB
    data += struct.pack(">I", 0)  # colour mode data
    data += struct.pack(">I", 0)  # image resources
    data += struct.pack(">I", 0)  # layer and mask info (none: flattened file)
    data += struct.pack(">H", 0)  # image data compression: raw
    for channel in range(3):  # planar: all R, then all G, then all B
        data += bytes(colour_at(x, y)[channel] for y in range(H) for x in range(W))
    write_atomic(os.path.join(out, "still.psd"), bytes(data))


# ---- SVG ---------------------------------------------------------------------------------------
def make_svg(out):
    hw, hh = W // 2, H // 2
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
  <rect x="0" y="0" width="{hw}" height="{hh}" fill="#ff0000"/>
  <rect x="{hw}" y="0" width="{hw}" height="{hh}" fill="#00ff00"/>
  <rect x="0" y="{hh}" width="{hw}" height="{hh}" fill="#0000ff"/>
  <rect x="{hw}" y="{hh}" width="{hw}" height="{hh}" fill="#ffffff"/>
</svg>
"""
    write_atomic(os.path.join(out, "still.svg"), svg.encode())


def quadrant_ppm():
    """The test picture as binary PPM (input for ffmpeg)."""
    pixels = bytes(c for y in range(H) for x in range(W) for c in colour_at(x, y))
    return b"P6\n%d %d\n255\n" % (W, H) + pixels


# ---- HEIC: ISOBMFF 'heic' file with one hvc1 item, ispe + irot properties ---------------------
#
# ffmpeg encodes the picture as a one-frame HEVC MP4; we take the hvcC configuration box and the
# single sample (length-prefixed NAL units, i.e. exactly what an hvc1 item stores) from it and
# wrap them in the HEIF meta structure. irot angle 1 = rotate 90 deg ANTI-clockwise to display,
# so a correct loader returns a portrait picture with red at the BOTTOM-left.
def box(kind, payload):
    return struct.pack(">I4s", 8 + len(payload), kind) + payload


def full_box(kind, version, flags, payload):
    return box(kind, struct.pack(">I", (version << 24) | flags) + payload)


def find_box(data, kind, start=0, end=None):
    """Returns (payload_start, payload_end) of the first top-level/nested box of `kind`."""
    pos = data.find(kind, start, end)
    if pos < 4:
        raise RuntimeError(f"box {kind!r} not found")
    size = struct.unpack(">I", data[pos - 4 : pos])[0]
    return pos + 4, pos - 4 + size


def make_heic(out):
    if not shutil.which("ffmpeg"):
        raise RuntimeError("ffmpeg not found")
    with tempfile.TemporaryDirectory() as tmp:
        ppm = os.path.join(tmp, "in.ppm")
        mp4 = os.path.join(tmp, "out.mp4")
        with open(ppm, "wb") as f:
            f.write(quadrant_ppm())
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", ppm,
             "-frames:v", "1", "-c:v", "libx265", "-pix_fmt", "yuv420p",
             "-x265-params", "log-level=error:pools=2", "-tag:v", "hvc1", mp4],
            check=True)
        with open(mp4, "rb") as f:
            src = f.read()
    hs, he = find_box(src, b"hvcC")
    hvcc = box(b"hvcC", src[hs:he])
    ms, me = find_box(src, b"mdat")
    sample = src[ms:me]

    ftyp = box(b"ftyp", b"heic" + struct.pack(">I", 0) + b"mif1heic")
    hdlr = full_box(b"hdlr", 0, 0, struct.pack(">I4s12s", 0, b"pict", b"\0" * 12) + b"\0")
    pitm = full_box(b"pitm", 0, 0, struct.pack(">H", 1))
    infe = full_box(b"infe", 2, 0, struct.pack(">HH4s", 1, 0, b"hvc1") + b"\0")
    iinf = full_box(b"iinf", 0, 0, struct.pack(">H", 1) + infe)
    ispe = full_box(b"ispe", 0, 0, struct.pack(">II", W, H))
    irot = box(b"irot", struct.pack(">B", 1))
    ipco = box(b"ipco", hvcc + ispe + irot)
    # property indices are 1-based; 0x80 = essential
    ipma = full_box(b"ipma", 0, 0, struct.pack(">IHB", 1, 1, 3) + bytes([0x81, 0x02, 0x83]))
    iprp = box(b"iprp", ipco + ipma)

    def build(offset):
        iloc = full_box(b"iloc", 0, 0, struct.pack(">BBH", 0x44, 0x00, 1)
                        + struct.pack(">HHHII", 1, 0, 1, offset, len(sample)))
        return full_box(b"meta", 0, 0, hdlr + pitm + iloc + iinf + iprp)

    meta = build(0)
    offset = len(ftyp) + len(meta) + 8  # mdat payload starts after its 8-byte header
    meta = build(offset)
    write_atomic(os.path.join(out, "still.heic"), ftyp + meta + box(b"mdat", sample))


# ---- JPEG with EXIF orientation 6 (rotate 90 deg clockwise to display) -----------------------
def make_exif_jpeg(out):
    from PIL import Image  # optional dependency

    image = Image.new("RGB", (W, H))
    image.putdata([colour_at(x, y) for y in range(H) for x in range(W)])
    exif = Image.Exif()
    exif[0x0112] = 6
    tmp = os.path.join(out, "tmp_exif_rot90.jpg")
    image.save(tmp, "JPEG", quality=92, exif=exif.tobytes())
    os.replace(tmp, os.path.join(out, "exif_rot90.jpg"))


GENERATORS = {
    "dng": make_dng,
    "psd": make_psd,
    "svg": make_svg,
    "heic": make_heic,
    "exifjpeg": make_exif_jpeg,
}


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for kind in sys.argv[2:]:
        try:
            GENERATORS[kind](out)
        except Exception as error:  # report and continue: the test skips this format
            print(f"make_test_images.py: skipping {kind}: {error}", file=sys.stderr)


if __name__ == "__main__":
    main()
