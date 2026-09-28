#!/usr/bin/env python3
"""Generates the tablet item texture.

Kept as a script rather than a committed binary blob so the art can be adjusted by editing named
colours and re-running, instead of by hand-editing pixels. It is deliberately a plain 16x16
front-facing device: item textures are viewed square-on, so a "held at an angle" tablet would read
as noise at this size.

    python tools/make_tablet_texture.py

Writes to common/src/main/resources/assets/remote-worker/textures/item/tablet.png and prints an
ASCII preview, because a 16x16 image is easier to judge in a terminal than in an image viewer.
"""

import struct
import sys
import zlib
from pathlib import Path

SIZE = 16

# A deliberately narrow palette: a handful of greys for the body, two blues for the screen, and one
# accent. More colours than this at 16x16 turns to mush once the texture is scaled up in game.
BODY = (0x3C, 0x40, 0x48)        # charcoal shell
BODY_LIGHT = (0x55, 0x5A, 0x64)   # bevel, top and left
BODY_DARK = (0x24, 0x27, 0x2C)    # bevel, bottom and right
SCREEN = (0x10, 0x18, 0x2E)       # the remote desktop, dark
SCREEN_BAR = (0x2E, 0x44, 0x6B)   # a window bar in the remote desktop
SCREEN_TEXT = (0x8A, 0xB4, 0xD8)  # something legible on it
BEZEL = (0x1A, 0x1C, 0x20)        # the gap between shell and screen
ACCENT = (0x64, 0xB0, 0xE8)       # the camera dot, and the power pip

# Rows 0-15. Each row is a string; one character per pixel.
#   . transparent   b body   B body light   d body dark   k bezel
#   s screen        w screen bar          t screen text   a accent
ART = [
    "................",
    "..bbbbbbbbbbbb..",
    ".bBbbbbbbbbbbBd.",
    ".bBkksssssskkBd.",
    ".bBksssssssssBd.",
    ".bBksswwwwsssBd.",
    ".bBkswttwtsssBd.",
    ".bBksssssssssBd.",
    ".bBksttttttskBd.",
    ".bBksssssssssBd.",
    ".bBkkkssssskkBd.",
    ".bddaaaaaaaaaabd",
    ".ddddddddddddddd",
    ".ddddddddddddddd",
    "................",
    "................",
]


def build_pixels():
    """Turns ART into an RGBA pixel grid, validating the shape as it goes."""
    if len(ART) != SIZE:
        raise SystemExit(f"ART has {len(ART)} rows, expected {SIZE}")
    palette = {
        ".": None,
        "b": BODY,
        "B": BODY_LIGHT,
        "d": BODY_DARK,
        "k": BEZEL,
        "s": SCREEN,
        "w": SCREEN_BAR,
        "t": SCREEN_TEXT,
        "a": ACCENT,
    }
    rows = []
    for y, row in enumerate(ART):
        if len(row) != SIZE:
            raise SystemExit(f"ART row {y} has {len(row)} characters, expected {SIZE}")
        pixels = []
        for c in row:
            if c not in palette:
                raise SystemExit(f"ART row {y} has unknown character {c!r}")
            colour = palette[c]
            pixels.append((0, 0, 0, 0) if colour is None else (*colour, 255))
        rows.append(pixels)
    return rows


def write_png(path, rows):
    """Writes RGBA rows as a PNG. Hand-rolled so the tool needs nothing but the standard library."""
    raw = b"".join(b"\x00" + b"".join(bytes(p) for p in row) for row in rows)

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", header)
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main():
    rows = build_pixels()
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        "versions/1.21.1/remote-worker/common/src/main/resources/assets/remote-worker"
        "/textures/item/tablet.png")
    write_png(out, rows)

    # The preview, so the result can be judged without opening an image viewer.
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    print()
    for row in ART:
        print("  " + row)
    print()
    opaque = sum(1 for row in rows for p in row if p[3] != 0)
    print(f"  {opaque}/{SIZE * SIZE} pixels opaque, {SIZE * SIZE - opaque} transparent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
