#!/usr/bin/env python3
"""Maps libqemu.so[+off] lines of a simpleperf report to function names from the qemu .debug file.

usage: qemu_hot.py <qemu.debug> <perf_symbols.txt> [top]
"""
import re
import struct
import sys
from bisect import bisect_right
from collections import defaultdict


def symbols(path):
    data = open(path, 'rb').read()
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
    secs = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    out = []
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for i in range(s[5] // 24):
            name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', data, s[4] + i * 24)
            if (info & 0xf) != 2 or value == 0:  # STT_FUNC
                continue
            o = strtab[4] + name
            out.append((value, size, data[o:data.index(b'\0', o)].decode()))
    out.sort()
    return out


def main():
    syms = symbols(sys.argv[1])
    starts = [s[0] for s in syms]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    agg = defaultdict(float)
    by_thread = defaultdict(float)
    for line in open(sys.argv[2]):
        m = re.match(r'\s*([\d.]+)%\s+(\S+)\s+\S*libqemu\.so\s+libqemu\.so\[\+([0-9a-f]+)\]', line)
        if not m:
            continue
        pct, comm, off = float(m.group(1)), m.group(2), int(m.group(3), 16)
        i = bisect_right(starts, off) - 1
        name = syms[i][2] if i >= 0 and off < syms[i][0] + max(syms[i][1], 1) else '?%x' % off
        agg[(comm, name)] += pct
        by_thread[comm] += pct
    print('resolved per thread:', {k: round(v, 2) for k, v in by_thread.items()})
    for (comm, name), pct in sorted(agg.items(), key=lambda kv: -kv[1])[:top]:
        print('%6.2f%%  %-16s %s' % (pct, comm, name))


main()
