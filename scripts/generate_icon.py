#!/usr/bin/env python3
"""Generates the SideScopes application icon.

Renders a vectorscope motif - graticule ring, skin-tone line, and a warm
trace cloud with the cursor marker on it - onto a Big Sur style rounded
square, then assembles assets/icon/sidescopes.icns. Pure Python on purpose:
no image libraries to install, and the icon stays reproducible from source.

Usage: python3 scripts/generate_icon.py  (from the repository root; needs
macOS sips and iconutil)
"""

import math
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

SIZE = 1024
SUPERSAMPLE = 2  # 2x2 samples per pixel

CENTER = SIZE / 2
CONTENT_HALF = 412  # Big Sur icon grid: content square inside the canvas
CORNER_RADIUS = 185
RING_RADIUS = 265
RING_WIDTH = 7.0
# The skin-tone line direction, up and to the left like BT.601 I-bar.
SKIN = (-0.62, -0.785)
SKIN_LENGTH = 0.72 * RING_RADIUS
MARKER_AT = 0.6 * RING_RADIUS


def rounded_square_coverage(x, y):
    """1 inside the rounded square, 0 outside, soft within two pixels."""
    dx = abs(x - CENTER) - (CONTENT_HALF - CORNER_RADIUS)
    dy = abs(y - CENTER) - (CONTENT_HALF - CORNER_RADIUS)
    if dx <= 0 and dy <= 0:
        return 1.0
    distance = math.hypot(max(dx, 0.0), max(dy, 0.0)) - CORNER_RADIUS
    return max(0.0, min(1.0, 0.5 - distance / 2.0))


def sample(x, y):
    coverage = rounded_square_coverage(x, y)
    if coverage <= 0.0:
        return (0.0, 0.0, 0.0, 0.0)

    px = x - CENTER
    py = y - CENTER
    radius = math.hypot(px, py)

    # Background: near-black with a whisper of vertical gradient.
    t = y / SIZE
    r = 0.075 - 0.020 * t
    g = 0.075 - 0.020 * t
    b = 0.086 - 0.020 * t

    # Graticule ring and a fainter inner ring.
    for ring_radius, strength in ((RING_RADIUS, 0.42), (RING_RADIUS * 0.62, 0.18)):
        edge = abs(radius - ring_radius) - RING_WIDTH / 2
        ring = max(0.0, min(1.0, 0.5 - edge / 2.0)) * strength
        r += ring * 0.62
        g += ring * 0.62
        b += ring * 0.66

    # Skin-tone line from the center outward, fading toward its tip.
    along = px * SKIN[0] + py * SKIN[1]
    across = abs(px * SKIN[1] - py * SKIN[0])
    if 0.0 <= along <= SKIN_LENGTH:
        edge = across - 2.6
        fade = 1.0 - 0.6 * along / SKIN_LENGTH
        line = max(0.0, min(1.0, 0.5 - edge / 2.0)) * 0.5 * fade
        r += line * 0.85
        g += line * 0.66
        b += line * 0.45

    # The trace cloud: a warm comet along the skin line, dense near the
    # center like a real portrait's vectorscope. The sigmoid keeps the
    # tail from cutting off across the center.
    behind = 1.0 / (1.0 + math.exp(-along / 22.0))
    along_out = max(along, 0.0)
    falloff = math.exp(-((across / (34.0 + along_out * 0.16)) ** 2))
    density = behind * falloff * math.exp(-along_out / (SKIN_LENGTH * 0.55)) * 1.15
    if density > 0.003:
        heat = min(1.0, density)
        r += density * (0.95 + 0.05 * heat)
        g += density * (0.42 + 0.30 * heat)
        b += density * (0.18 + 0.16 * heat)

    # The cursor marker: a small white ring riding on the trace.
    marker_x = SKIN[0] * MARKER_AT
    marker_y = SKIN[1] * MARKER_AT
    marker_distance = math.hypot(px - marker_x, py - marker_y)
    edge = abs(marker_distance - 13.0) - 2.6
    marker = max(0.0, min(1.0, 0.5 - edge / 2.0))
    r += marker * 0.92
    g += marker * 0.92
    b += marker * 0.92

    return (min(r, 1.0), min(g, 1.0), min(b, 1.0), coverage)


def render():
    rows = []
    step = 1.0 / SUPERSAMPLE
    offsets = [step * (i + 0.5) for i in range(SUPERSAMPLE)]
    samples = SUPERSAMPLE * SUPERSAMPLE
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            r = g = b = a = 0.0
            for oy in offsets:
                for ox in offsets:
                    sr, sg, sb, sa = sample(x + ox, y + oy)
                    r += sr * sa
                    g += sg * sa
                    b += sb * sa
                    a += sa
            a /= samples
            if a > 0.0:
                r /= samples * a
                g /= samples * a
                b /= samples * a
            row += bytes(
                (
                    round(r * a * 255),  # PNG wants non-premultiplied; a is
                    round(g * a * 255),  # applied here because the samples
                    round(b * a * 255),  # were accumulated premultiplied.
                    round(a * 255),
                )
            )
        rows.append(bytes(row))
        if y % 128 == 0:
            print(f"  row {y}/{SIZE}", file=sys.stderr)
    return rows


def write_png(path, rows):
    def chunk(kind, payload):
        data = kind + payload
        return struct.pack(">I", len(payload)) + data + struct.pack(">I", zlib.crc32(data))

    raw = b"".join(b"\x00" + row for row in rows)
    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    icon_dir = root / "assets" / "icon"
    icon_dir.mkdir(parents=True, exist_ok=True)

    print("rendering master 1024x1024", file=sys.stderr)
    master = icon_dir / "sidescopes-1024.png"
    write_png(master, render())

    with tempfile.TemporaryDirectory() as scratch:
        iconset = pathlib.Path(scratch) / "sidescopes.iconset"
        iconset.mkdir()
        for points in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                pixels = points * scale
                suffix = "" if scale == 1 else "@2x"
                target = iconset / f"icon_{points}x{points}{suffix}.png"
                subprocess.run(
                    ["sips", "-z", str(pixels), str(pixels), str(master), "--out", str(target)],
                    check=True,
                    capture_output=True,
                )
        subprocess.run(
            ["iconutil", "-c", "icns", str(iconset), "-o", str(icon_dir / "sidescopes.icns")],
            check=True,
        )
    print(f"wrote {icon_dir / 'sidescopes.icns'}", file=sys.stderr)


if __name__ == "__main__":
    main()
