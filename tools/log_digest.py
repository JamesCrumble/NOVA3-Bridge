#!/usr/bin/env python3
"""Digest of a game log: most frequent line shapes, error-ish lines, glprof/fps trend. log_digest.py <log>"""
import re
import sys
from collections import Counter

lines = open(sys.argv[1], errors='replace').read().splitlines()
shape = Counter()
errs = Counter()
fps, prof = [], []
for l in lines:
    s = re.sub(r'0x[0-9a-fA-F]+', 'X', l)
    s = re.sub(r'\d+(\.\d+)?', 'N', s)[:160]
    shape[s] += 1
    if re.search(r'(?i)error|fail|warn|fatal|unknown|unsupported|not supported|no decoder|reject|abort|assert|missing|denied|SIG', l):
        errs[s] += 1
    m = re.search(r'frames ([\d.]+)/s.*?busy (\d+)%.*?per frame: (.*?) \|', l)
    if m:
        fps.append(float(m.group(1)))
    m = re.search(r'glprof: step ([\d.]+) ms', l)
    if m:
        prof.append(float(m.group(1)))
print('lines', len(lines))
print('\n== top line shapes')
for s, n in shape.most_common(25):
    print('%7d  %s' % (n, s))
print('\n== error-ish')
for s, n in errs.most_common(40):
    print('%7d  %s' % (n, s))
if fps:
    print('\n== fps reports: n=%d min %.1f max %.1f median %.1f' % (len(fps), min(fps), max(fps), sorted(fps)[len(fps) // 2]))
    print('   last 30:', ' '.join('%.0f' % f for f in fps[-30:]))
if prof:
    print('== step ms: n=%d median %.1f max %.1f' % (len(prof), sorted(prof)[len(prof) // 2], max(prof)))
