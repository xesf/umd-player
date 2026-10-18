#!/usr/bin/env python3
"""Research tool for UMD Video menus: reads the RCO files in UMD_VIDEO/RESOURCE/.

    rcodump.py <file.rco>... [-t] [-s] [-d] [-x outdir]
        -t  print the resource tree (pages, widgets, images, animations)
        -s  print the script's string tables (names, properties, text)
        -d  disassemble the script (opcode names are partly guesses; see docs/research/umd-menus.md)
        -x  extract the images (PNG) and the script (VSMX) to outdir
    With none of -t, -s, -d, it prints a summary and the script's use of the player objects.

    rcodump.py --survey <iso>...   extract every RCO from the discs (with build/host/umdinfo) and
                                   print what all their scripts use, disc by disc

Standard library only. The layout comes from inspecting the files on our test discs, checked
against what is publicly known about the PSP's RCO format; nothing is copied from other tools.
"""
import collections
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
UMDINFO = os.path.join(HERE, '..', 'build', 'host', 'umdinfo')

TYPES = {1: 'main', 2: 'script', 3: 'text', 4: 'image', 5: 'sound', 6: 'model', 7: 'font', 8: 'object', 9: 'anim'}

# Script opcodes. The ones marked (?) are inferred from how they are used, not confirmed.
OPS = {
    0x01: 'assign', 0x02: 'add', 0x03: 'sub', 0x04: 'mul', 0x05: 'div', 0x06: 'mod', 0x08: 'neg (?)',
    0x07: 'unary plus', 0x09: 'not', 0x0a: 'pre inc', 0x0b: 'pre dec', 0x0c: 'inc (?)', 0x0d: 'dec (?)', 0x0e: 'eq (?)', 0x0f: 'ne (?)', 0x10: 'strict eq (?)',
    0x12: 'lt', 0x13: 'le', 0x14: 'ge', 0x15: 'gt', 0x19: 'and (?)', 0x20: 'dup (?)',
    0x22: 'end statement', 0x23: 'undefined (?)', 0x24: 'null (?)', 0x25: 'bool', 0x26: 'int', 0x27: 'float',
    0x28: 'string', 0x2a: 'function', 0x2b: 'array (?)', 0x2c: 'this (?)', 0x2d: 'local', 0x2e: 'name',
    0x2f: 'property', 0x30: 'method', 0x31: 'set property', 0x34: 'get item', 0x35: 'get item for call',
    0x36: 'set item', 0x38: 'array push', 0x39: 'jump', 0x3a: 'jump if true', 0x3b: 'jump if false',
    0x3c: 'call function', 0x3d: 'call method', 0x3e: 'new', 0x3f: 'return', 0x45: 'end of script',
    0x29: 'object', 0x33: 'object property', 0x32: 'delete property', 0x37: 'delete item', 0x46: 'line marker', 0x47: 'line'}

# The objects the PSP's UMD Video player gives the scripts.
HOST_OBJECTS = ('movieplayer', 'controller', 'resource', 'timer', 'sound', 'Array', 'Math', 'Date', 'String')


class Rco:
    def __init__(self, data):
        self.d = data
        if data[:4] != b'\0PRF':
            raise ValueError('not an RCO file')
        self.version = struct.unpack_from('<I', data, 4)[0]
        p_label, l_label = struct.unpack_from('<II', data, 0x40)
        self.labels = data[p_label:p_label + l_label]
        self.p_img_data = struct.unpack_from('<I', data, 0x80)[0]
        self.entries = []          # (depth, type, subtype, label, offset, header fields)
        self.walk(0xA4, 0)

    def label(self, off):
        if off == 0xFFFFFFFF or off >= len(self.labels):
            return ''
        end = self.labels.find(b'\0', off)
        return self.labels[off:end].decode('latin1')

    def walk(self, off, depth):
        sub, typ, _, lab, ehs, esz, nsub, nxt, _, _ = struct.unpack_from('<BBHIIIIIII', self.d, off)
        self.entries.append((depth, TYPES.get(typ, 't%x' % typ), sub, self.label(lab), off, esz))
        child = off + (esz if esz else ehs)
        for _ in range(nsub):
            n = self.walk(child, depth + 1)
            if not n:
                break
            child += n
        return nxt

    def scripts(self):
        for depth, typ, sub, name, off, esz in self.entries:
            if typ == 'script' and sub:
                start, length = struct.unpack_from('<II', self.d, off + 0x28)
                base = off + 0x30 + start
                yield name, Vsmx(self.d[base:base + length])

    def images(self):
        for depth, typ, sub, name, off, esz in self.entries:
            if typ == 'image' and sub:
                fmt, comp, size, start = struct.unpack_from('<HHII', self.d, off + 0x28)
                yield name, fmt, comp, self.d[self.p_img_data + start:self.p_img_data + start + size]


class Vsmx:
    def __init__(self, data):
        if data[:4] != b'VSMX':
            raise ValueError('not a VSMX script')
        (_, _, co, cl, to, tl, tn, po, pl, pn, no, nl, nn) = struct.unpack_from('<4sI11I', data, 0)
        self.data = data
        self.code = [struct.unpack_from('<II', data, co + i * 8) for i in range(cl // 8)]
        self.text = data[to:to + tl].decode('utf-16le').split('\0')[:tn]
        self.props = data[po:po + pl].decode('utf-16le').split('\0')[:pn]
        self.names = data[no:no + nl].decode('latin1').split('\0')[:nn]

    def operand(self, op, val):
        table = {0x2e: self.names, 0x2f: self.props, 0x30: self.props, 0x31: self.props, 0x28: self.text}.get(op)
        if table is not None and val < len(table):
            return repr(table[val])
        if op == 0x27:
            return '%g' % struct.unpack('<f', struct.pack('<I', val))[0]
        if op & 0xFF == 0x2a:
            return 'at %d, %d args, flags %x' % (val, op >> 24, (op >> 16) & 0xFF)
        return str(val)

    def uses(self):
        """'object.member' for every member read, called or set right after a name."""
        out = collections.Counter()
        for (op, val), (op2, val2) in zip(self.code, self.code[1:]):
            if op == 0x2e and op2 in (0x2f, 0x30, 0x31) and val < len(self.names) and val2 < len(self.props):
                out['%s.%s' % (self.names[val], self.props[val2])] += 1
        return out

    def members(self):
        """Every property name used in any way, with how often it is called, read and set."""
        out = collections.defaultdict(lambda: [0, 0, 0])
        for op, val in self.code:
            if op in (0x30, 0x2f, 0x31) and val < len(self.props):
                out[self.props[val]][{0x30: 0, 0x2f: 1, 0x31: 2}[op]] += 1
        return out


def dump(path, args):
    rco = Rco(open(path, 'rb').read())
    print('== %s (RCO version %#x, %d entries)' % (path, rco.version, len(rco.entries)))
    if '-t' in args:
        for depth, typ, sub, name, off, esz in rco.entries:
            print('%s%s.%x %s' % ('  ' * depth, typ, sub, name))
    kinds = collections.Counter((typ, sub) for _, typ, sub, _, _, _ in rco.entries if sub)
    print('entries:', ', '.join('%s.%x %d' % (t, s, n) for (t, s), n in sorted(kinds.items())))
    for name, vs in rco.scripts():
        print('script %s: %d instructions, %d names, %d properties, %d strings' %
              (name, len(vs.code), len(vs.names), len(vs.props), len(vs.text)))
        if '-s' in args:
            print('  names:', vs.names)
            print('  properties:', vs.props)
            print('  text:', vs.text)
        if '-d' in args:
            for i, (op, val) in enumerate(vs.code):
                print('  %5d  %-16s %s' % (i, OPS.get(op & 0xFF if op & 0xFF == 0x2a else op, '? %x' % op),
                                          vs.operand(op, val)))
        if not ({'-t', '-s', '-d'} & set(args)):
            for use, n in sorted(vs.uses().items()):
                if use.split('.')[0] in HOST_OBJECTS:
                    print('  %-40s %d' % (use, n))
    if '-x' in args:
        out = args[args.index('-x') + 1]
        os.makedirs(out, exist_ok=True)
        stem = os.path.splitext(os.path.basename(path))[0]
        for name, fmt, comp, data in rco.images():
            ext = 'png' if data[:4] == b'\x89PNG' else 'img%d' % fmt
            open(os.path.join(out, '%s_%s.%s' % (stem, name, ext)), 'wb').write(data)
        for name, vs in rco.scripts():
            open(os.path.join(out, '%s_%s.vsmx' % (stem, name)), 'wb').write(vs.data)


def survey(isos):
    every = collections.Counter()
    members = collections.Counter()
    with tempfile.TemporaryDirectory() as tmp:
        for iso in isos:
            listing = subprocess.run([UMDINFO, iso, '-f'], capture_output=True, text=True).stdout
            rcos = [line.split()[-1] for line in listing.splitlines() if line.strip().endswith('.RCO')]
            disc = collections.Counter()
            for i, path in enumerate(rcos):
                local = os.path.join(tmp, '%d.rco' % i)
                with open(local, 'wb') as f:
                    subprocess.run([UMDINFO, iso, '-x', path], stdout=f, check=True)
                rco = Rco(open(local, 'rb').read())
                for _, vs in rco.scripts():
                    for use, n in vs.uses().items():
                        if use.split('.')[0] in HOST_OBJECTS:
                            disc[use] += n
                    for m in vs.members():
                        members[m] += 1
            print('== %s: %d RCO files' % (os.path.basename(iso), len(rcos)))
            for use in sorted(disc):
                print('  ' + use)
                every[use] += 1
    print('== host object members used, and on how many discs')
    for use, n in sorted(every.items()):
        print('  %-40s %d' % (use, n))


def main():
    args = sys.argv[1:]
    if args and args[0] == '--survey':
        survey(args[1:])
        return
    for path in [a for a in args if a.lower().endswith('.rco')]:
        dump(path, args)


if __name__ == '__main__':
    main()
