#!/usr/bin/env python3
"""Summarises the "fps:" lines of a player log (ux0:data/umdplayer/log.txt on the Vita).
    fpslog.py log.txt [threshold]
Prints the overall rate, the worst picture gap, the stretches below the threshold (default 95% of
the typical rate, since the stream's own timing can be 29.97 for a 24 fps film with pulldown) with their film time, and the CPU mode they were in. Standard library only."""
import re
import sys

LINE = re.compile(r'fps: ([\d.]+) of ([\d.]+)\s+picture every ([\d.]+) ms \(95% ([\d.]+), worst ([\d.]+)\)\s+skipped (\d+)\s+'
                  r'per second: decode (\d+) ms, present (\d+) ms, update (\d+) ms, slowest update ([\d.]+) ms\s+'
                  r'cpu (\w+)\s+audio (\w+)\s+t ([\d.]+) s')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    rows = [LINE.search(l) for l in open(sys.argv[1], errors='replace')]
    rows = [m.groups() for m in rows if m]
    if not rows:
        sys.exit('no "fps:" lines in the log')
    rate = [float(r[0]) for r in rows]
    nominal = float(rows[0][1])
    typical = sorted(rate)[len(rate) // 2]
    limit = float(sys.argv[2]) if len(sys.argv) > 2 else typical * 0.97
    skipped = sum(int(r[5]) for r in rows)
    print('%d intervals of 2 s, stream timing %.2f fps, typical rate %.2f fps' % (len(rows), nominal, typical))
    print('mean %.2f fps, lowest %.2f fps, skipped pictures %d' % (sum(rate) / len(rate), min(rate), skipped))
    print('worst gap between pictures: %.1f ms at t %s s' % max((float(r[4]), r[12]) for r in rows))
    print('slowest update: %.1f ms' % max(float(r[9]) for r in rows))
    modes = {}
    for r in rows:
        modes[r[10]] = modes.get(r[10], 0) + 1
    print('cpu mode: ' + ', '.join('%s %d intervals' % kv for kv in sorted(modes.items())))
    low = [r for r in rows if float(r[0]) < limit]
    print('%d intervals below %.1f fps:' % (len(low), limit))
    for r in low[:40]:
        print('  t %7s s  %5s fps  gap 95%% %s ms, worst %s ms  decode %s, present %s, update %s ms/s  skipped %s  cpu %s'
              % (r[12], r[0], r[3], r[4], r[6], r[7], r[8], r[5], r[10]))
    if len(low) > 40:
        print('  ... and %d more' % (len(low) - 40))


main()
