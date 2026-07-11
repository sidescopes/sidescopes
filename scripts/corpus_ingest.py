#!/usr/bin/env python3
"""Ingests a screenshot into the detection corpus.

Subsamples every second pixel - exact values, no blending, matching the
debug dump transform - so a Retina capture lands at point resolution with
its contrast steps intact, and writes a sidecar skeleton next to it.

Usage: python3 scripts/corpus_ingest.py <screenshot.png> corpus/<case-name>
"""

import pathlib
import struct
import sys
import zlib


def decode_png(path):
    """Decodes an 8-bit non-interlaced RGB(A) PNG - what screenshots are."""
    data = pathlib.Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "expected a PNG screenshot"
    pos, width, height, depth, color = 8, 0, 0, 0, 0
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", payload[:10])
            assert depth == 8 and color in (2, 6), "expected 8-bit RGB(A) PNG"
            assert payload[12] == 0, "interlaced PNG unsupported; re-save the screenshot"
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break
        pos += 12 + length
    channels = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    pos = 0
    for _ in range(height):
        filter_type = raw[pos]
        line = bytearray(raw[pos + 1 : pos + 1 + stride])
        pos += 1 + stride
        if filter_type == 1:  # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + predictor) & 0xFF
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 1
    source = pathlib.Path(sys.argv[1])
    target = pathlib.Path(sys.argv[2])
    width, height, channels, rows = decode_png(source)

    out_width, out_height = width // 2, height // 2
    body = bytearray()
    for y in range(out_height):
        row = rows[y * 2]
        for x in range(out_width):
            offset = x * 2 * channels
            body += row[offset : offset + 3]
    ppm = target.with_suffix(".ppm")
    ppm.write_bytes(b"P6\n%d %d\n255\n" % (out_width, out_height) + bytes(body))

    sidecar = target.with_suffix(".txt")
    if not sidecar.exists():
        sidecar.write_text(
            "pixels_per_point 1\n"
            "# mask x y w h        (optional, repeatable)\n"
            "# expect x y w h tolerance\n"
        )
    print(f"wrote {ppm} ({out_width}x{out_height}) and {sidecar}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
