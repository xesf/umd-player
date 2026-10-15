#!/usr/bin/env python3
"""Draws the Vita manual: the pages the LiveArea's Manual button shows.

    python3 -I tools/make_manual.py [out_dir]     (default: assets/vita/manual)

The pages are 960x544 indexed PNGs named 001.png, 002.png and so on; CMakeLists.txt puts
them in the VPK as sce_sys/manual/. They follow docs/manual.md, on the film-strip backdrop
of the LiveArea, with screenshots from docs/manual/. Standard library only; the drawing
helpers come from tools/make_assets.py.
"""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_assets as A  # noqa: E402

ROOT = os.path.dirname(HERE)
W, H = 960, 544
BAND = 58                    # film strip height, as on the app's backdrop
LEFT = 40
BODY = (0.88, 0.91, 0.97)
DIM = (0.62, 0.70, 0.84)


def read_png(path):
    """8-bit RGB or RGBA, not interlaced (what sips writes). Returns (w, h, rows of RGB)."""
    d = open(path, 'rb').read()
    w, h, depth, ctype = struct.unpack('>IIBB', d[16:26])
    assert depth == 8 and ctype in (2, 6), 'unsupported PNG'
    bpp = 4 if ctype == 6 else 3
    data, i = b'', 8
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]
        if d[i + 4:i + 8] == b'IDAT':
            data += d[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(data)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(w)])
        prev = line
    return w, h, rows


def put_half(c, path, x0, y0):
    """The screenshot at half size (2x2 averaged), with a thin frame."""
    w, h, rows = read_png(path)
    c.rect(x0 - 3, y0 - 3, w // 2 + 6, h // 2 + 6, (0.45, 0.70, 1.00))
    for y in range(h // 2):
        for x in range(w // 2):
            s = [0, 0, 0]
            for dy in (0, 1):
                for dx in (0, 1):
                    p = rows[2 * y + dy][2 * x + dx]
                    for k in range(3):
                        s[k] += p[k]
            c.px[(y0 + y) * c.w + x0 + x] = [v / 1020.0 for v in s]


def wrap(text, chars):
    out, line = [], ''
    for word in text.split(' '):
        if line and len(line) + 1 + len(word) > chars:
            out.append(line)
            line = word
        else:
            line = word if not line else line + ' ' + word
    if line:
        out.append(line)
    return out


class Page:
    def __init__(self, base, number, title):
        self.c = A.Canvas(W, H, ss=1)
        self.c.px = [p[:] for p in base.px]
        self.y = BAND + 22
        self.c.text(title, LEFT, self.y, 3, A.WHITE)
        self.c.rect(LEFT, self.y + 30, A.text_width(title, 3), 3, A.ACCENT)
        n = '%d / %d' % (number, PAGES)
        self.c.text(n, W - LEFT - A.text_width(n, 1), H - BAND - 16, 1, DIM)
        self.y += 52

    def para(self, text, chars=55, color=BODY, gap=8):
        for line in wrap(text, chars):
            self.c.text(line, LEFT, self.y, 2, color)
            self.y += 20
        self.y += gap

    def rows(self, items, key_w=150, chars=40):
        """Button, then what it does, wrapped beside it."""
        for key, what in items:
            self.c.text(key, LEFT, self.y, 2, A.ACCENT)
            for line in wrap(what, chars):
                self.c.text(line, LEFT + key_w, self.y, 2, BODY)
                self.y += 20
            self.y += 4

    def save(self, out, number):
        A.save(self.c, os.path.join(out, '%03d.png' % number))


PAGES = 10


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'assets', 'vita', 'manual')
    shots = os.path.join(ROOT, 'docs', 'manual')
    os.makedirs(out, exist_ok=True)
    base = A.Canvas(W, H, ss=3)
    A.film_background(base, BAND, 36, 24, 64)
    version = 'v' + A.app_version()

    # 1: cover
    c = A.Canvas(W, H, ss=3)
    c.px = [p[:] for p in base.px]
    A.emblem(c, 300, 272, 190)
    c.text('UMD Player', 420, 222, 5, A.WHITE)
    c.rect(420, 266, A.text_width('UMD Player', 5), 4, A.ACCENT)
    c.text('Manual', 420, 284, 3, BODY)
    c.text('by xesf  ' + version, 420, 324, 2, DIM)
    A.save(c, os.path.join(out, '001.png'))

    p = Page(base, 2, 'Install and add movies')
    p.para('UMD Player plays UMD Video discs (the movies, episodes and concerts sold on UMD for '
           'the PSP) from disc images, without Adrenaline.')
    p.para('Turn on "Enable unsafe homebrew" in the HENkaku settings. Adrenaline needs it too.')
    p.para('Copy .iso, .cso or .zso images of UMD Video discs to either folder:')
    p.rows([('', 'ux0:umd/'), ('', 'ux0:pspemu/ISO/VIDEO/')], key_w=40)
    p.y += 6
    p.para('A PSP with custom firmware can dump a UMD over USB. Only UMD Video discs play, not PSP '
           'games. With no movies, the library shows these folders; press Cross to look again.')
    p.save(out, 2)

    p = Page(base, 3, 'The library')
    put_half(p.c, os.path.join(shots, 'library.png'), W - LEFT - 480, p.y)
    p.rows([('Up/Down', 'Choose a disc'), ('Cross', 'Play, from where you stopped'),
            ('Square', 'Play from the start'), ('Select', 'Disc name and resume point'),
            ('Triangle', 'Settings'), ('PS', 'Leave the app')], key_w=150, chars=13)
    p.y = max(p.y, BAND + 74 + 272 + 16)
    p.para('Discs show their own background, animated icon and music, as on a PSP.')
    p.save(out, 3)

    p = Page(base, 4, 'Settings')
    put_half(p.c, os.path.join(shots, 'settings.png'), W - LEFT - 480, p.y)
    p.para('Triangle in the library. Up/Down chooses a row, Left/Right changes it, Circle closes.',
           chars=25)
    p.rows([('Start', 'Movie: play at once. Disc: logos, warnings and the disc\'s menus first.'),
            ('Screen', 'Picture size'), ('Audio', 'Language'), ('Subs', 'Off, System or a language')],
           key_w=116, chars=15)
    p.y = max(p.y, BAND + 74 + 272 + 16)
    p.para('Screen, Audio and Subtitles apply to discs you have not played yet. A disc you watched '
           'keeps its own choices.')
    p.save(out, 4)

    p = Page(base, 5, 'Watching a movie')
    p.rows([('Cross/Start', 'Pause or play'),
            ('Left/Right', 'Back or forward 10 s; hold to rewind or fast-forward faster'),
            ('Up/Down', 'Next or previous chapter'),
            ('L/R', 'Previous or next title on the disc (episodes, extras)'),
            ('Square', 'Next screen mode: Fit, Zoom, Fill, Stretch'),
            ('Triangle', 'Options panel'), ('Select', 'Time bar'), ('Circle', 'Back to the library'),
            ('Touch', 'Tap for the time bar; slide along it and lift to jump there')], key_w=190)
    p.save(out, 5)

    p = Page(base, 6, 'Time bar and options')
    put_half(p.c, os.path.join(shots, 'player-options.png'), W - LEFT - 480, p.y)
    p.para('The time bar shows the disc, the position and the length.', chars=25)
    p.para('The options panel (Triangle) changes audio and subtitles while the movie plays, the '
           'screen mode and the chapter.', chars=25)
    p.y = max(p.y, BAND + 74 + 272 + 16)
    p.para('Up/Down: row. Left/Right: change. Cross on Chapter: go there. Circle: close. The panel '
           'also shows the date, time and battery.')
    p.save(out, 6)

    p = Page(base, 7, 'Disc menus')
    p.para('With Start with: Disc, a disc plays as on a PSP: its logos and warnings, its own menu '
           'screens and choices, then the movie. A disc without menus plays its intros first.')
    p.rows([('D-pad', 'Move between menu items'), ('Cross', 'Choose'),
            ('Circle', 'Back in the menus; the disc\'s menu during the movie'),
            ('Select', 'Back to the library (or hold Circle)'),
            ('Touch', 'Tap an item to choose it; swipe to move')], key_w=130)
    p.y += 8
    p.para('A disc you leave saves its own place, as on a PSP, and continues from there next time. '
           'Square in the library starts it from the beginning.')
    p.save(out, 7)

    p = Page(base, 8, 'Interactive movies (UMD-PG)')
    p.para('UMD-PG discs, such as Summer Days, are interactive movies: the story stops at choices, '
           'and what you choose decides what plays next. They always play through their own menus.')
    p.rows([('D-pad', 'Move between choices, or password digits'),
            ('Cross', 'Choose; during the story, pause or play'),
            ('Circle', 'During the story: show the password for this point'),
            ('Select', 'Back to the library; the disc continues from here next time')], key_w=130)
    p.save(out, 8)

    p = Page(base, 9, 'Passwords')
    p.para('A game on several discs carries your progress to the next disc with a 4-digit password. '
           'A password also takes you back to a point you reached before.')
    p.rows([('1', 'During the story, press Circle. Write down the password and the disc it is for.'),
            ('2', 'Pause with Cross to hide it, and Cross again to play.'),
            ('3', 'On that disc, choose the title menu option that loads a game (on discs 2 and 3 '
                  'of Summer Days, the start option).'),
            ('4', 'Up/Down change a digit, Left/Right move. Choose OK with Cross.')], key_w=40, chars=48)
    p.save(out, 9)

    p = Page(base, 10, 'Saved data and help')
    p.para('UMD Player remembers where you stopped each disc and its audio, subtitle and screen '
           'settings, where you left each disc played through its menus, and your settings, in '
           'ux0:data/umdplayer/. Deleting a file resets it.')
    p.rows([('Empty', 'Check the folders and the .iso, .cso or .zso names'),
            ('pspemu', 'Missing movies there: enable unsafe homebrew'),
            ('Old icon', 'After an update: delete and install again'),
            ('Freezes', 'Send ux0:data/umdplayer/log.txt'),
            ('Menus', 'If a disc\'s menus misbehave, use Start with: Movie')], key_w=150)
    p.save(out, 10)


if __name__ == '__main__':
    main()
