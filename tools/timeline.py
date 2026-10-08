#!/usr/bin/env python3
"""Time series from a game log: per bridge report fps, draws, vertex MB/frame, uniforms, and the last step() time.
If fps falls while the per-frame work stays flat, the CPU got slower (clocks); if the work grows, the scene did."""
import re
import sys

step = None
menu = '?'
print('  #   fps  draws  vtxKB/f  unif  stepms  menu  busy%  qemu-core')
k = 0
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'glprof: step ([\d.]+) ms', l)
    if m:
        step = float(m.group(1))
        continue
    m = re.search(r'phase=\w+ frame=\d+ menu=(\d)', l)
    if m:
        menu = m.group(1)
        continue
    m = re.search(r'frames ([\d.]+)/s.*?busy (\d+)%.*?per frame: (.*?) \| bytes/frame: (.*)', l)
    if not m:
        continue
    cnt = dict((int(a), int(b)) for a, b in re.findall(r'(\d+)=(\d+)', m.group(3)))
    byt = dict((int(a), int(b)) for a, b in re.findall(r'(\d+)=(\d+)', m.group(4)))
    k += 1
    c = re.search(r'\| (cpu\d+ \d+/\d+ MHz)', l)
    print('%3d %5.1f %6d %8.0f %5d %7s %5s %5s  %s' % (k, float(m.group(1)), cnt.get(21, 0) + cnt.get(20, 0),
          byt.get(21, 0) / 1024.0, cnt.get(18, 0), '%.1f' % step if step else '-', menu, m.group(2),
          c.group(1) if c else ''))
