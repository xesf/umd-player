#!/usr/bin/env python3
"""Draws the Vita bubble icon and LiveArea images for UMD Player.

    python3 -I tools/make_assets.py [out_dir]     (default: assets/vita)

Standard library only. The emblem is an original drawing: a disc in a PSP-style caddy
(the physical shape only, without Sony's label, wordmark, or logos) with a film frame in front. Text uses the app's 8x8 font (src/core/font8x8.c). The Vita wants 8-bit
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


def app_version():
    src = open(os.path.join(ROOT, 'src', 'app', 'version.h')).read()
    m = re.search(r'#define UMDPLAYER_VERSION "([^"]+)"', src)
    return m.group(1) if m else '?'


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


# ---- the emblem: a disc in a PSP-style caddy, and a film frame in front ----

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


def hsv(h, s, v):
    i = int(h * 6) % 6
    f = h * 6 - int(h * 6)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    return [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]


def caddy(cx, cy, rs):
    """The caddy outline around a disc centred at (cx, cy): a half circle of radius rs on top,
    straight sides, rounded bottom corners, and a slightly convex bottom edge."""
    rc = rs * 0.20                       # bottom corner radius
    base = cy + rs * 0.96                # bottom edge at the sides
    bulge = rs * 0.07                    # how far the middle of the bottom edge drops

    def bottom(x):
        d = (x - cx) / rs
        return base + bulge * (1 - d * d)

    def inside(x, y):
        dx = x - cx
        if y <= cy:
            return dx * dx + (y - cy) ** 2 <= rs * rs
        if abs(dx) > rs or y > bottom(x):
            return False
        # round the bottom corners
        kx = cx + math.copysign(rs - rc, dx)
        ky = bottom(kx) - rc
        if abs(dx) > rs - rc and y > ky:
            return (x - kx) ** 2 + (y - ky) ** 2 <= rc * rc
        return True
    return inside, bottom(cx)


def emblem(c, cx, cy, size):
    """size is the caddy width. The disc centre is at (cx, cy)."""
    rs = size / 2.0
    r = rs * 0.90                        # disc radius
    outline, lowest = caddy(cx, cy, rs)
    box = (cx - rs - 2, cy - rs - 2, cx + rs + 2, lowest + 2)

    # soft shadow, then the light grey shell with a top-to-bottom shade
    sh, _ = caddy(cx + size * 0.015, cy + size * 0.03, rs)
    c.shape(sh, (0.0, 0.0, 0.03), 0.45, bbox=(box[0], box[1], box[2] + size * 0.02, box[3] + size * 0.04))
    c.shape(outline, lambda x, y: mix((0.96, 0.96, 0.97), (0.80, 0.81, 0.84), (y - (cy - rs)) / (lowest - (cy - rs))),
            bbox=box)
    # the window rim around the disc
    c.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= (r * 1.035) ** 2, (0.62, 0.64, 0.68), bbox=box)

    # the disc: silver with a faint rainbow sheen that turns around the centre
    def disc(x, y):
        ang = math.atan2(y - cy, x - cx)
        rainbow = hsv(((ang / (2 * math.pi)) * 2) % 1.0, 0.22, 1.0)
        light = 0.5 + 0.5 * math.cos(2 * ang + 0.6)
        base = mix((0.80, 0.82, 0.86), (0.99, 0.99, 1.00), light)
        return mix(base, rainbow, 0.55)
    c.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r, disc, bbox=box)
    # the centre label: plain, with a thin edge
    lr = r * 0.40
    c.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= (lr * 1.04) ** 2, (0.70, 0.72, 0.76), bbox=box)
    c.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= lr * lr,
            lambda x, y: mix((0.99, 0.99, 1.0), (0.86, 0.87, 0.90), (y - (cy - lr)) / (2 * lr)), bbox=box)

    # the film frame, over the lower right of the disc
    fw, fh = size * 0.50, size * 0.34
    fx, fy = cx + rs * 0.12, cy + rs * 0.40
    frame = rounded_rect(fx, fy, fx + fw, fy + fh, size * 0.025)
    c.shape(rounded_rect(fx + size * 0.01, fy + size * 0.02, fx + fw + size * 0.01, fy + fh + size * 0.02, size * 0.025),
            (0.0, 0.0, 0.03), 0.4, bbox=(fx - 2, fy - 2, fx + fw + size * 0.02, fy + fh + size * 0.03))
    c.shape(frame, (0.07, 0.08, 0.11), bbox=(fx - 2, fy - 2, fx + fw + 2, fy + fh + 2))
    holes = 5
    hw, hh = fw / (holes * 2.0), fh * 0.12
    for i in range(holes):
        hx = fx + fw * (i + 0.5) / holes - hw / 2
        for hy in (fy + fh * 0.06, fy + fh - fh * 0.06 - hh):
            c.shape(rounded_rect(hx, hy, hx + hw, hy + hh, hh * 0.3), WHITE, 0.9, bbox=(hx - 1, hy - 1, hx + hw + 1, hy + hh + 1))
    px0, py0, px1, py1 = fx + fw * 0.08, fy + fh * 0.26, fx + fw * 0.92, fy + fh * 0.74
    c.shape(rounded_rect(px0, py0, px1, py1, size * 0.01),
            lambda x, y: mix(ACCENT, (0.20, 0.35, 0.80), (y - py0) / (py1 - py0)), bbox=(px0 - 1, py0 - 1, px1 + 1, py1 + 1))
    tcx, tcy, th = (px0 + px1) / 2, (py0 + py1) / 2, (py1 - py0) * 0.55

    def triangle(x, y):
        return x >= tcx - th * 0.4 and abs(y - tcy) <= (tcx + th * 0.5 - x) * 0.58
    c.shape(triangle, WHITE, bbox=(tcx - th, tcy - th, tcx + th, tcy + th))


# ---- 8-bit indexed PNG ----

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


DITHER_LIGHT = 8      # one 5-bit step: hides banding
DITHER_STRONG = 40    # five steps: a visible pattern, for the LiveArea


def quantize(c, colors=256, dither=DITHER_LIGHT, origin=(0, 0)):
    """Median cut on 5-bit-per-channel colours, then nearest-colour mapping, after a 4x4
    ordered dither of the given strength. origin places the dither grid, so images that are
    shown next to each other share one grid."""
    px = []
    ox, oy = origin
    for i, p in enumerate(c.px):
        d = (BAYER[(i // c.w + oy) % 4][(i % c.w + ox) % 4] / 16.0 - 0.5) * dither
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


def save(c, path, dither=DITHER_LIGHT, origin=(0, 0)):
    palette, idx = quantize(c, dither=dither, origin=origin)
    write_png(path, c.w, c.h, palette, idx)
    print('%s: %dx%d, %d colours' % (path, c.w, c.h, len(palette)))


# ---- the three images ----

def icon0(path):
    c = Canvas(128, 128)
    background(c)
    emblem(c, 62, 58, 88)
    save(c, path)


def film_background(c, band, hole_w, hole_h, step):
    """A film strip: dark base, sprocket holes along the top and bottom bands, and the navy
    picture area between them, like the film frame in the emblem."""
    gx, gy = c.w * 0.3, c.h * 0.35
    diag = math.hypot(c.w, c.h)

    def fn(x, y):
        if y < band or y >= c.h - band:
            return (0.035, 0.04, 0.055)
        t = min(1.0, math.hypot(x - gx, y - gy) / (diag * 0.7))
        return mix(NAVY_LIGHT, NAVY, t)
    c.fill(fn)
    # a thin edge between the strip and the picture
    c.rect(0, band - 2, c.w, 2, (0.12, 0.14, 0.20))
    c.rect(0, c.h - band, c.w, 2, (0.12, 0.14, 0.20))
    count = int(c.w // step)
    x0 = (c.w - count * step) / 2 + (step - hole_w) / 2
    for i in range(count):
        hx = x0 + i * step
        for hy in ((band - hole_h) / 2, c.h - band + (band - hole_h) / 2):
            c.shape(rounded_rect(hx, hy, hx + hole_w, hy + hole_h, hole_h * 0.25), (0.86, 0.88, 0.92),
                    bbox=(hx - 1, hy - 1, hx + hole_w + 1, hy + hole_h + 1))


def bg(path):
    """LiveArea background. With style a1 the start image (280x158) and its Start button sit in
    the middle, about x 280-560 and y 170-400; the title group sits at the lower left."""
    c = Canvas(840, 500, ss=3)
    film_background(c, 54, 34, 22, 60)
    title, by, scale = 'UMD Player', 'by xesf', 3
    icon, gap = 64, 18
    version_y = 500 - 54 - 8 - 16        # the version line, just above the bottom strip
    group_h = 46 + 16                    # from the group's top to the bottom of "by xesf"
    x, y = 15, version_y + 16 - group_h  # bottoms aligned with the version
    emblem(c, x + icon * 0.48, y + 32, icon * 0.86)
    tx = x + icon + gap
    c.text(title, tx, y + 8, scale, WHITE)
    c.rect(tx, y + 36, text_width(title, scale), 3, ACCENT)
    c.text(by, tx, y + 46, 2, (0.70, 0.78, 0.92))
    # the version at the far right, just above the bottom strip
    version = 'v' + app_version()
    c.text(version, 840 - 16 - text_width(version, 2), version_y, 2, (0.70, 0.78, 0.92))
    save(c, path, DITHER_STRONG)


def startup(path):
    """The start image: the emblem on the same gradient the LiveArea background has where the
    image sits (x 280, y 171 with style a1), so the two blend without a visible box."""
    c = Canvas(280, 158, ss=3)
    gx, gy, diag = 840 * 0.3, 500 * 0.35, math.hypot(840, 500)
    c.fill(lambda x, y: mix(NAVY_LIGHT, NAVY, min(1.0, math.hypot(x + 280 - gx, y + 171 - gy) / (diag * 0.7))))
    emblem(c, 136, 68, 112)
    save(c, path, DITHER_STRONG, origin=(280, 171))


def backdrop(png_path, c_path):
    """The app's own backdrop, for the empty library and while a movie loads: the LiveArea
    film strip at screen size, without text. Also written as a C array so the app has it
    built in."""
    c = Canvas(960, 544, ss=3)
    film_background(c, 58, 36, 24, 64)
    save(c, png_path, DITHER_STRONG)
    data = open(png_path, 'rb').read()
    lines = ['/* Generated by tools/make_assets.py from assets/app/backdrop.png. Do not edit. */',
             '#include "backdrop.h"', '',
             'const unsigned int backdrop_png_size = %d;' % len(data),
             'const unsigned char backdrop_png[] = {']
    for i in range(0, len(data), 16):
        lines.append('    ' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',')
    lines.append('};')
    open(c_path, 'w').write('\n'.join(lines) + '\n')


TEMPLATE = '''<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="26">
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
    if len(sys.argv) <= 1:
        os.makedirs(os.path.join(ROOT, 'assets', 'app'), exist_ok=True)
        backdrop(os.path.join(ROOT, 'assets', 'app', 'backdrop.png'),
                 os.path.join(ROOT, 'src', 'app', 'backdrop_png.c'))
    print(os.path.join(live, 'template.xml'))


if __name__ == '__main__':
    main()
