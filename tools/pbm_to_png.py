#!/usr/bin/env python3
"""Turn the 1-bit PBM files rendered by render_faces into viewable PNGs.

Golden baselines prove a face has not changed. They cannot tell you whether it
looks good. This exists for that judgement.

    python3 tools/pbm_to_png.py <indir> [--scale N] [--sheet out.png] [--cols N]

Writes one PNG per PBM, and optionally a contact sheet with all of them side by
side, which is the form worth actually looking at.

No third-party dependencies: PNG is written directly with zlib.
"""

import argparse
import glob
import os
import struct
import sys
import zlib


def read_pbm(path):
    """Read a binary PBM (P4). Returns (w, h, rows) where rows[y][x] is 0/1."""
    with open(path, "rb") as f:
        data = f.read()

    # Header is P4 then width then height, whitespace separated, with '#'
    # comments legal anywhere between tokens.
    pos = 0
    tokens = []
    while len(tokens) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos : pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        tokens.append(data[start:pos])
    pos += 1  # exactly one whitespace byte after the last token

    if tokens[0] != b"P4":
        raise ValueError(f"{path}: not a binary PBM (got {tokens[0]!r})")
    w, h = int(tokens[1]), int(tokens[2])

    stride = (w + 7) // 8
    body = data[pos : pos + stride * h]
    if len(body) < stride * h:
        raise ValueError(f"{path}: truncated ({len(body)} < {stride * h} bytes)")

    rows = []
    for y in range(h):
        base = y * stride
        row = [(body[base + (x >> 3)] >> (7 - (x & 7))) & 1 for x in range(w)]
        rows.append(row)
    return w, h, rows


def write_png(path, w, h, rows, scale=1, invert=False):
    """Greyscale-8 PNG. 1 in the PBM means ink, which renders black."""
    out = bytearray()
    for row in rows:
        for _ in range(scale):
            out.append(0)  # filter: none
            for v in row:
                ink = v if not invert else (1 - v)
                px = 0 if ink else 255
                out.extend(bytes([px]) * scale)

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(out), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def contact_sheet(images, cols, gap=12, label_h=0):
    """Lay images out in a grid on a mid-grey field so the panel edges show."""
    if not images:
        return None
    cw = max(w for _, w, _, _ in images)
    ch = max(h for _, _, h, _ in images)
    rows_n = (len(images) + cols - 1) // cols
    W = cols * cw + (cols + 1) * gap
    H = rows_n * (ch + label_h) + (rows_n + 1) * gap

    # 0 = ink, 1 = paper in our convention; use 2 for the surrounding field and
    # map it separately so the panel outline is visible against it.
    canvas = [[2] * W for _ in range(H)]
    for i, (_, w, h, rows) in enumerate(images):
        r, c = divmod(i, cols)
        x0 = gap + c * (cw + gap)
        y0 = gap + r * (ch + label_h + gap)
        for y in range(h):
            for x in range(w):
                canvas[y0 + y][x0 + x] = rows[y][x]
    return W, H, canvas


def write_sheet_png(path, W, H, canvas, scale=1):
    out = bytearray()
    # PBM convention: 1 = ink. The panel is transmissive and backlit, so ink is
    # dark on a bright field -- 1 must render BLACK, matching write_png(). These
    # two maps disagreeing is an easy way to spend an afternoon deciding a face
    # looks wrong when it is only inverted.
    shade = {0: 255, 1: 0, 2: 110}  # paper, ink, surround
    for row in canvas:
        for _ in range(scale):
            out.append(0)
            for v in row:
                out.extend(bytes([shade[v]]) * scale)

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W * scale, H * scale, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(out), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("indir")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--sheet")
    ap.add_argument("--cols", type=int, default=4)
    ap.add_argument("--only", default="", help="substring filter on filename")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.indir, "*.pbm")))
    if args.only:
        paths = [p for p in paths if args.only in os.path.basename(p)]
    if not paths:
        print(f"no .pbm files in {args.indir}", file=sys.stderr)
        return 1

    images = []
    for p in paths:
        w, h, rows = read_pbm(p)
        out = p[:-4] + ".png"
        write_png(out, w, h, rows, scale=args.scale)
        images.append((os.path.basename(p)[:-4], w, h, rows))
        print(f"wrote {out}  ({w}x{h} -> {w*args.scale}x{h*args.scale})")

    if args.sheet:
        W, H, canvas = contact_sheet(images, args.cols)
        write_sheet_png(args.sheet, W, H, canvas, scale=args.scale)
        print(f"wrote {args.sheet}  ({W*args.scale}x{H*args.scale}, {len(images)} panels)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
