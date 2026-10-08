#!/usr/bin/env python3
"""Where an ELF32 refers to a string by absolute pointer: xref_str.py <lib> <string> [context_words]
Prints the string's vaddr and every aligned word in the loaded segments equal to it, with neighbours
(each neighbour that is itself a pointer to a string is shown as that string)."""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
target = sys.argv[2].encode() + b'\0'
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 6
phoff, = struct.unpack_from('<I', data, 0x1C)
phentsize, phnum = struct.unpack_from('<HH', data, 0x2A)
loads = []
for i in range(phnum):
    t, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from('<IIIIIIII', data, phoff + i * phentsize)
    if t == 1:
        loads.append((off, vaddr, filesz))


def off2va(o):
    for off, va, sz in loads:
        if off <= o < off + sz:
            return va + o - off
    return None


def va2off(v):
    for off, va, sz in loads:
        if va <= v < va + sz:
            return off + v - va
    return None


def cstr(v):
    o = va2off(v)
    if o is None:
        return None
    e = data.find(b'\0', o, o + 80)
    if e < 0 or e == o:
        return None
    s = data[o:e]
    return s.decode('latin1') if all(32 <= c < 127 for c in s) else None


starts = []
p = data.find(target)
while p >= 0:
    if p == 0 or data[p - 1] == 0:
        starts.append(p)
    p = data.find(target, p + 1)
for so in starts:
    sva = off2va(so)
    print('string at va 0x%x' % sva)
    for off, va, sz in loads:
        for o in range(off, off + sz - 3, 4):
            if struct.unpack_from('<I', data, o)[0] == sva:
                wva = va + o - off
                print('  ref at va 0x%x:' % wva)
                for k in range(-ctx, ctx + 1):
                    w = struct.unpack_from('<I', data, o + 4 * k)[0]
                    s = cstr(w)
                    print('    %+3d  0x%08x  %s' % (k, w, repr(s) if s else ''))
