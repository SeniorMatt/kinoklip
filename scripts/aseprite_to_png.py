#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Renders the first frame of an Aseprite file (.aseprite / .ase) to a PNG, without Aseprite.

    scripts/aseprite_to_png.py in.aseprite out.png [--scale N]

Supports what the project's art uses: RGBA colour mode, normal layers and groups (hidden
layers and the children of hidden groups are skipped), layer and cel opacity with normal
blending, raw / compressed / linked cels. --scale N enlarges with nearest neighbour. Needs Pillow.
File format: https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md
"""
import argparse
import struct
import sys
import zlib

from PIL import Image

HEADER_MAGIC = 0xA5E0
FRAME_MAGIC = 0xF1FA
CHUNK_LAYER = 0x2004
CHUNK_CEL = 0x2005


def render(path: str) -> Image.Image:
    data = open(path, "rb").read()
    _, magic, frames, width, height, depth, flags = struct.unpack_from("<IHHHHHI", data, 0)
    if magic != HEADER_MAGIC:
        raise ValueError(f"{path}: not an Aseprite file")
    if depth != 32:
        raise ValueError(f"{path}: only RGBA files are supported (colour depth {depth})")
    layer_opacity_valid = bool(flags & 1)

    layers = []  # dicts: visible (incl. parents), opacity, blend
    cels = {}  # layer index -> (x, y, opacity, image)
    offset = 128
    for frame in range(1 if frames else 0):
        frame_size, frame_magic, old_chunks = struct.unpack_from("<IHH", data, offset)
        if frame_magic != FRAME_MAGIC:
            raise ValueError(f"{path}: bad frame header")
        chunks = struct.unpack_from("<I", data, offset + 12)[0] or old_chunks
        pos = offset + 16
        parents = []  # visibility per child level
        for _ in range(chunks):
            chunk_size, chunk_type = struct.unpack_from("<IH", data, pos)
            body = pos + 6
            if chunk_type == CHUNK_LAYER:
                lflags, ltype, level, _, _, blend, opacity = struct.unpack_from("<HHHHHHB", data, body)
                visible = bool(lflags & 1) and all(parents[:level])
                parents[level:] = [visible]
                layers.append({"visible": visible, "type": ltype, "blend": blend,
                               "opacity": opacity if layer_opacity_valid else 255})
            elif chunk_type == CHUNK_CEL:
                index, x, y, opacity, cel_type = struct.unpack_from("<HhhBH", data, body)
                cel_body = body + 16
                if cel_type in (0, 2):
                    w, h = struct.unpack_from("<HH", data, cel_body)
                    raw = data[cel_body + 4: pos + chunk_size]
                    pixels = raw if cel_type == 0 else zlib.decompress(raw)
                    cels[index] = (x, y, opacity, Image.frombytes("RGBA", (w, h), pixels[: w * h * 4]))
                elif cel_type != 1:  # 1: linked cel, same content (only frame 0 is rendered)
                    raise ValueError(f"{path}: unsupported cel type {cel_type}")
            pos += chunk_size
        offset += frame_size

    out = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    for index, layer in enumerate(layers):
        if index not in cels or not layer["visible"] or layer["type"] != 0:
            continue
        if layer["blend"] != 0:
            print(f"warning: layer {index} uses blend mode {layer['blend']}, drawn as normal",
                  file=sys.stderr)
        x, y, opacity, image = cels[index]
        alpha = opacity * layer["opacity"] // 255
        if alpha < 255:
            image = image.copy()
            image.putalpha(image.getchannel("A").point(lambda a: a * alpha // 255))
        layer_image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        layer_image.paste(image, (x, y))
        out = Image.alpha_composite(out, layer_image)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--scale", type=int, default=1)
    args = parser.parse_args()
    image = render(args.input)
    if args.scale > 1:
        image = image.resize((image.width * args.scale, image.height * args.scale), Image.NEAREST)
    image.save(args.output, optimize=True)
    print(f"{args.output}: {image.width} x {image.height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
