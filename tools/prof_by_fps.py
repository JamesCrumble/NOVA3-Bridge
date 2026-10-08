#!/usr/bin/env python3
"""Splits a log's guest-profile reports by the average FPS of the 10 s before each: prof_by_fps.py <log> <fps_limit>
Prints the aggregate profile of reports below / at-or-above the limit (uses guest_prof.py's symbolization)."""
import re, sys, subprocess, os
log = sys.argv[1]; lim = float(sys.argv[2])
here = os.path.dirname(os.path.abspath(__file__))
lines = open(log, errors='replace').read().splitlines()
fps = []; low = []; high = []; n = 0
for l in lines:
    m = re.match(r'\[gl\] frames ([\d.]+)/s', l)
    if m: fps.append(float(m.group(1)))
    if l.startswith('QEMU_GUEST_PROF:'):
        avg = sum(fps) / len(fps) if fps else 0
        (low if avg < lim else high).append(n); n += 1; fps = []
print('reports below %.0f fps: %s' % (lim, low)); print('above: %s' % high)
open('/tmp/_split.txt', 'w').write(' '.join(map(str, low)) + '\n' + ' '.join(map(str, high)))
