#!/usr/bin/env python3
"""Draws the Vita bubble icon and LiveArea images for UMD Player.

    python3 -I tools/make_assets.py [out_dir]     (default: assets/vita)

Standard library only. The emblem is an original drawing: a disc in a UMD-style shell with a
film frame in front. Text uses the app's 8x8 font (src/core/font8x8.c). The Vita wants 8-bit
indexed PNGs, so images are reduced to 256 colours (median cut) before writing.
"""
import math
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


# ---- font ----

def load_font():
    src = open(os.path.join(ROOT, 'src', 'core', 'font8x8.c')).read()
    rows = re.findall(r'\{((?:\s*0x[0-9A-Fa-f]{2},?){8})\s*\}', src)
    return [[int(v, 16) for v in re.findall(r'0x[0-9A-Fa-f]{2}', r)] for r in rows]


FONT = load_font()


# ---- a tiny RGBA canvas with supersampled shapes ----

class Canvas:
    def __init__(self, w, h, ss=4):
        self.w, self.h, self.ss = w, h, ss
        self.px = [[0.0, 0.0, 0.0] for _ in range(w * h)]

    def fill(self, fn):
        """fn(x, y) -> (r, g, b) in 0..1, for every pixel centre."""
        for y in range(self.h):
            for x in range(self.w):
                self.px[y * self.w + x] = list(fn(x + 0.5, y + 0.5))

    def shape(self, inside, color, alpha=1.0, bbox=None):
        """Blends color where inside(x, y) is true, with supersampled edges.
        color may be a tuple or fn(x, y) -> tuple."""
        x0, y0, x1, y1 = bbox or (0, 0, self.w, self.h)
        x0, y0 = max(0, int(x0)), max(0, int(y0))
        x1, y1 = min(self.w, int(math.ceil(x1))), min(self.h, int(math.ceil(y1)))
        n = self.ss
        for y in range(y0, y1):
            for x in range(x0, x1):
                hits = 0
                for sy in range(n):
                    for sx in range(n):
                        if inside(x + (sx + 0.5) / n, y + (sy + 0.5) / n):
                            hits += 1
                if not hits:
                    continue
                a = alpha * hits / (n * n)
                c = color(x + 0.5, y + 0.5) if callable(color) else color
                p = self.px[y * self.w + x]
                for k in range(3):
                    p[k] = p[k] * (1 - a) + c[k] * a

    def text(self, s, x, y, scale, color, shadow=True):
        for i, ch in enumerate(s):
            glyph = FONT[ord(ch) if ord(ch) < 128 else ord('?')]
            for gy in range(8):
                for gx in range(8):
                    if glyph[gy] & (1 << gx):
                        px, py = x + (i * 8 + gx) * scale, y + gy * scale
                        if shadow:
                            self.rect(px + scale // 3 + 1, py + scale // 3 + 1, scale, scale, (0.02, 0.03, 0.08), 0.6)
                        self.rect(px, py, scale, scale, color)
        return len(s) * 8 * scale

    def rect(self, x, y, w, h, color, alpha=1.0):
        for yy in range(max(0, y), min(self.h, y + h)):
            for xx in range(max(0, x), min(self.w, x + w)):
                p = self.px[yy * self.w + xx]
                for k in range(3):
                    p[k] = p[k] * (1 - alpha) + color[k] * alpha


def text_width(s, scale):
    return len(s) * 8 * scale


def mix(a, b, t):
    return tuple(a[k] + (b[k] - a[k]) * t for k in range(3))


def rounded_rect(x0, y0, x1, y1, r):
    def inside(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return inside


# ---- the emblem: a UMD-style shell with a disc, and a film frame in front ----

NAVY = (0.04, 0.07, 0.16)
NAVY_LIGHT = (0.10, 0.18, 0.38)
ACCENT = (0.45, 0.70, 1.00)
WHITE = (0.95, 0.96, 0.98)


def background(c):
    """Deep navy with a soft glow at the upper left."""
    gx, gy = c.w * 0.3, c.h * 0.3
    diag = math.hypot(c.w, c.h)

    def fn(x, y):
        t = min(1.0, math.hypot(x - gx, y - gy) / (diag * 0.75))
        return mix(NAVY_LIGHT, NAVY, t)
    c.fill(fn)


def emblem(c, cx, cy, size):
    """size is the shell width. The shell is a rounded square with a slightly narrower,
    rounded bottom edge, like a UMD caddy; the disc shows through a round window."""
    s = size / 2.0
    shell = rounded_rect(cx - s, cy - s, cx + s, cy + s * 1.04, s * 0.22)
    c.shape(shell, lambda x, y: mix((0.20, 0.24, 0.32), (0.08, 0.10, 0.16), (y - (cy - s)) / (2 * s)),
            bbox=(cx - s - 2, cy - s - 2, cx + s + 2, cy + s * 1.04 + 2))
    # a thin light rim on the shell
    rim = lambda x, y: shell(x, y) and not rounded_rect(cx - s + size * 0.02, cy - s + size * 0.02, cx + s - size * 0.02,
                                                        cy + s * 1.04 - size * 0.02, s * 0.20)(x, y)
    c.shape(rim, (0.55, 0.62, 0.75), 0.8, bbox=(cx - s - 2, cy - s - 2, cx + s + 2, cy + s * 1.04 + 2))

    # the disc: silver with a soft sheen, darker inner ring, and the spindle hole
    r = s * 0.80
    dcx, dcy = cx, cy + s * 0.02

    def silver(x, y):
        ang = math.atan2(y - dcy, x - dcx)
        sheen = 0.5 + 0.5 * math.cos(2 * ang - 0.8)
        return mix((0.55, 0.60, 0.68), (0.93, 0.95, 0.98), sheen)
    c.shape(lambda x, y: (x - dcx) ** 2 + (y - dcy) ** 2 <= r * r, silver, bbox=(dcx - r, dcy - r, dcx + r, dcy + r))
    c.shape(lambda x, y: (x - dcx) ** 2 + (y - dcy) ** 2 <= (r * 0.36) ** 2, (0.40, 0.45, 0.55),
            bbox=(dcx - r, dcy - r, dcx + r, dcy + r))
    c.shape(lambda x, y: (x - dcx) ** 2 + (y - dcy) ** 2 <= (r * 0.16) ** 2, (0.05, 0.07, 0.12),
            bbox=(dcx - r, dcy - r, dcx + r, dcy + r))

    # the film frame, overlapping the lower right
    fw, fh = size * 0.62, size * 0.42
    fx, fy = cx + s * 0.20, cy + s * 0.38
    frame = rounded_rect(fx, fy, fx + fw, fy + fh, size * 0.03)
    c.shape(frame, (0.07, 0.08, 0.11), bbox=(fx - 2, fy - 2, fx + fw + 2, fy + fh + 2))
    holes = 5
    hw, hh = fw / (holes * 2.0), fh * 0.12
    for i in range(holes):
        hx = fx + fw * (i + 0.5) / holes - hw / 2
        for hy in (fy + fh * 0.06, fy + fh - fh * 0.06 - hh):
            c.shape(rounded_rect(hx, hy, hx + hw, hy + hh, hh * 0.3), WHITE, 0.9, bbox=(hx - 1, hy - 1, hx + hw + 1, hy + hh + 1))
    # the picture area with a play mark
    px0, py0, px1, py1 = fx + fw * 0.08, fy + fh * 0.26, fx + fw * 0.92, fy + fh * 0.74
    c.shape(rounded_rect(px0, py0, px1, py1, size * 0.01),
            lambda x, y: mix(ACCENT, (0.20, 0.35, 0.80), (y - py0) / (py1 - py0)), bbox=(px0 - 1, py0 - 1, px1 + 1, py1 + 1))
    tcx, tcy, th = (px0 + px1) / 2, (py0 + py1) / 2, (py1 - py0) * 0.55

    def triangle(x, y):
        return x >= tcx - th * 0.4 and abs(y - tcy) <= (tcx + th * 0.5 - x) * 0.58
    c.shape(triangle, WHITE, bbox=(tcx - th, tcy - th, tcx + th, tcy + th))


# ---- 8-bit indexed PNG ----

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def quantize(c, colors=256):
    """Median cut on 5-bit-per-channel colours, then nearest-colour mapping. A 4x4 ordered
    dither of one 5-bit step first keeps the dark gradients from banding."""
    px = []
    for i, p in enumerate(c.px):
        d = (BAYER[(i // c.w) % 4][i % c.w % 4] / 16.0 - 0.5) * 8
        px.append(tuple(max(0, min(255, int(v * 255 + 0.5 + d))) for v in p))
    counts = {}
    for p in px:
        k = (p[0] >> 3, p[1] >> 3, p[2] >> 3)
        counts[k] = counts.get(k, 0) + 1
    boxes = [list(counts.items())]
    while len(boxes) < colors:
        boxes.sort(key=lambda b: max(max(e[0][i] for e in b) - min(e[0][i] for e in b) for i in range(3)) if len(b) > 1 else -1)
        b = boxes.pop()
        if len(b) < 2:
            boxes.append(b)
            break
        ch = max(range(3), key=lambda i: max(e[0][i] for e in b) - min(e[0][i] for e in b))
        b.sort(key=lambda e: e[0][ch])
        total, acc, cut = sum(e[1] for e in b), 0, 1
        for i, e in enumerate(b):
            acc += e[1]
            if acc >= total / 2:
                cut = max(1, min(len(b) - 1, i + 1))
                break
        boxes.append(b[:cut])
        boxes.append(b[cut:])
    palette = []
    for b in boxes:
        n = sum(e[1] for e in b)
        palette.append(tuple(int(sum((e[0][i] * 8 + 4) * e[1] for e in b) / n) for i in range(3)))
    cache = {}

    def nearest(p):
        k = (p[0] >> 3, p[1] >> 3, p[2] >> 3)
        if k not in cache:
            q = (k[0] * 8 + 4, k[1] * 8 + 4, k[2] * 8 + 4)
            cache[k] = min(range(len(palette)), key=lambda i: sum((palette[i][j] - q[j]) ** 2 for j in range(3)))
        return cache[k]
    return palette, [nearest(p) for p in px]


def write_png(path, w, h, palette, indices):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + bytes(indices[y * w:(y + 1) * w]) for y in range(h))
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 3, 0, 0, 0))
    png += chunk(b'PLTE', b''.join(bytes(c) for c in palette))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def save(c, path):
    palette, idx = quantize(c)
    write_png(path, c.w, c.h, palette, idx)
    print('%s: %dx%d, %d colours' % (path, c.w, c.h, len(palette)))


# ---- the three images ----

def icon0(path):
    c = Canvas(128, 128)
    background(c)
    emblem(c, 60, 58, 78)
    save(c, path)


def bg(path):
    c = Canvas(840, 500, ss=3)
    background(c)
    emblem(c, 230, 240, 300)
    x = 452
    c.text('UMD Player', x, 196, 4, WHITE)
    c.rect(x, 242, text_width('UMD Player', 4), 4, ACCENT)
    c.text('by xesf', x, 264, 3, (0.70, 0.78, 0.92))
    save(c, path)


def startup(path):
    c = Canvas(280, 158, ss=3)
    background(c)
    emblem(c, 62, 76, 86)
    c.text('UMD', 128, 52, 3, WHITE)
    c.text('Player', 128, 84, 3, WHITE)
    save(c, path)


TEMPLATE = '''<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
'''


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'assets', 'vita')
    live = os.path.join(out, 'livearea', 'contents')
    os.makedirs(live, exist_ok=True)
    icon0(os.path.join(out, 'icon0.png'))
    bg(os.path.join(live, 'bg.png'))
    startup(os.path.join(live, 'startup.png'))
    open(os.path.join(live, 'template.xml'), 'w').write(TEMPLATE)
    print(os.path.join(live, 'template.xml'))


if __name__ == '__main__':
    main()
