#!/usr/bin/env python3
"""Writes the stock colour grading LUTs to wadsrc/static/graphics.

Each LUT is a 32x32x32 cube packed as a 1024x32 strip: red increases across
each 32-pixel slice, green increases downwards and blue selects the slice.
Use r_grading_lut, MAPINFO 'gradinglut' or PPShader.SetGradingLUT with the
lump name (LUTNEUTR, LUTNIGHT). Artists can grade a screenshot together with
LUTNEUTR in an image editor and save the edited strip as a new LUT.

Usage: python3 scripts/grading_luts.py
"""

import os
import struct
import zlib

N = 32


def luma(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def mix(a, b, t):
    return a + (b - a) * t


def neutral(c):
    return c


def night(c):
    """Cinematic night: cool teal shadows, warm highlights, saturated reds,
    slightly deeper blacks and a gentle S-curve."""
    r, g, b = c
    # Keep reds (lamps, blood, warning lights) saturated, mute everything else a little.
    redness = min(max((r - max(g, b)) * 3.0, 0.0), 1.0)
    sat = mix(0.82, 1.2, redness)
    l = luma((r, g, b))
    r, g, b = (l + (x - l) * sat for x in (r, g, b))
    # Split toning.
    l = luma((r, g, b))
    shadow = 1.0 - smoothstep(0.0, 0.55, l)
    high = smoothstep(0.45, 1.0, l)
    tint_s = (0.86, 0.98, 1.14)
    tint_h = (1.07, 1.0, 0.9)
    r, g, b = (x * mix(1.0, ts, shadow) * mix(1.0, th, high) for x, ts, th in zip((r, g, b), tint_s, tint_h))
    # Gentle S-curve and slightly crushed blacks.
    out = []
    for x in (r, g, b):
        x = min(max(x, 0.0), 1.0)
        x = mix(x, x * x * (3.0 - 2.0 * x), 0.3)
        x = (x - 0.015) / 0.985
        out.append(min(max(x, 0.0), 1.0))
    return out


def write_png(path, width, height, rows):
    raw = b''.join(b'\x00' + bytes(row) for row in rows)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def make_lut(func):
    rows = []
    for y in range(N):
        row = []
        for x in range(N * N):
            c = ((x % N) / (N - 1), y / (N - 1), (x // N) / (N - 1))
            row.extend(int(round(min(max(v, 0.0), 1.0) * 255)) for v in func(c))
        rows.append(row)
    return rows


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'wadsrc', 'static', 'graphics')
    for name, func in (('LUTNEUTR', neutral), ('LUTNIGHT', night)):
        write_png(os.path.join(out, name + '.png'), N * N, N, make_lut(func))


if __name__ == '__main__':
    main()
