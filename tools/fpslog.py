#!/usr/bin/env python3
"""Summarises the "fps:" lines of a player log (ux0:data/umdplayer/log.txt on the Vita).
    fpslog.py log.txt [threshold]
Prints the overall rate, the worst picture gap, the stretches below the threshold (default 97% of
the typical rate, since the stream's own timing can be 29.97 for a 24 fps film with pulldown)
with their film time, the slowest calls in them, and the time in each CPU mode. Every title in
the log is summarised separately. Standard library only."""
import re
import sys

LINE = re.compile(
    r'fps: (?P<fps>[\d.]+) of (?P<nominal>[\d.]+)\s+picture every (?P<avg>[\d.]+) ms \(95% (?P<p95>[\d.]+), '
    r'worst (?P<worst>[\d.]+)\)\s+skipped (?P<skipped>\d+)\s+'
    r'per second: decode (?P<decode>\d+) ms, present (?P<present>\d+) ms, update (?P<update>\d+) ms, '
    r'slowest update (?P<slow>[\d.]+) ms\s+'
    r'(?:worst call: decode (?P<wdec>[\d.]+) ms, present (?P<wpres>[\d.]+) ms, save (?P<wsave>[\d.]+) ms\s+'
    r'caught up (?P<caught>\d+)\s+)?cpu (?P<cpu>\w+)\s+audio (?P<audio>\w+)\s+t (?P<t>[\d.]+) s')


def summary(rows, limit_arg):
    f = lambda k: [float(r[k]) for r in rows if r[k] is not None]
    rate = f('fps')
    typical = sorted(rate)[len(rate) // 2]
    limit = limit_arg if limit_arg else typical * 0.97
    print('%d intervals of 2 s, stream timing %s fps, typical rate %.2f fps' % (len(rows), rows[0]['nominal'], typical))
    print('mean %.2f fps, lowest %.2f fps, skipped pictures %d, shown late %d'
          % (sum(rate) / len(rate), min(rate), sum(int(r['skipped']) for r in rows), sum(int(r['caught'] or 0) for r in rows)))
    w = max(rows, key=lambda r: float(r['worst']))
    print('worst gap between pictures: %s ms at t %s s' % (w['worst'], w['t']))
    print('slowest update: %s ms; slowest decode %.1f ms, present %.1f ms, save %.1f ms'
          % (max(f('slow')), max(f('wdec') or [0]), max(f('wpres') or [0]), max(f('wsave') or [0])))
    modes = {}
    for r in rows:
        modes[r['cpu']] = modes.get(r['cpu'], 0) + 1
    print('cpu mode: ' + ', '.join('%s %d intervals' % kv for kv in sorted(modes.items())))
    low = [r for r in rows if float(r['fps']) < limit]
    print('%d intervals below %.1f fps:' % (len(low), limit))
    for r in low[:30]:
        print('  t %7s s  %5s fps  gap 95%% %s ms, worst %s ms  decode %s, present %s, update %s ms/s  skipped %s  late %s  cpu %s'
              % (r['t'], r['fps'], r['p95'], r['worst'], r['decode'], r['present'], r['update'], r['skipped'], r['caught'], r['cpu']))
    if len(low) > 30:
        print('  ... and %d more' % (len(low) - 30))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    groups, last = [], None
    for l in open(sys.argv[1], errors='replace'):
        if l.startswith('title "'):
            groups.append((l.strip(), []))
        m = LINE.search(l)
        if m and groups:
            groups[-1][1].append(m.groupdict())
    groups = [g for g in groups if len(g[1]) >= 5]
    if not groups:
        sys.exit('no "fps:" lines in the log')
    limit = float(sys.argv[2]) if len(sys.argv) > 2 else None
    for title, rows in groups:
        print('==', title)
        summary(rows, limit)


main()
