#!/usr/bin/env python3
"""How often synchronous bridge queries (ops 23..36) appear in the per-frame op reports of a log."""
import re
import sys

names = {23: 'GetString', 24: 'GetIntegerv', 25: 'GetFloatv', 26: 'GetShaderiv', 27: 'GetProgramiv',
         28: 'ShaderLog', 29: 'ProgramLog', 30: 'ShaderSource', 31: 'ActiveAttrib', 32: 'ActiveUniform',
         33: 'AttribLoc', 34: 'UniformLoc', 35: 'CheckFB', 36: 'ReadPixels'}
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'frames ([\d.]+)/s.*per frame: (.*?) \| bytes/frame: (.*)', l)
    if not m:
        continue
    cnt = dict((int(a), b) for a, b in re.findall(r'(\d+)=(\d+)', m.group(2)))
    byt = dict((int(a), b) for a, b in re.findall(r'(\d+)=(\d+)', m.group(3)))
    hits = ['%s x%s (%sB)' % (names[k], cnt.get(k, '?'), byt.get(k, '?'))
            for k in names if k in cnt or k in byt]
    if hits:
        print('%5s fps  %s' % (m.group(1), ', '.join(hits)))
