#!/usr/bin/env python3
"""String an ARM `ldr rX,[pc,#i]` + `add rX, pc, rX` pair points at: pcrel_str.py <lib> <ldr_va> <add_va> ..."""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<I', data, 0x1C)
phentsize, phnum = struct.unpack_from('<HH', data, 0x2A)
loads = []
for i in range(phnum):
    t, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from('<IIIIIIII', data, phoff + i * phentsize)
    if t == 1:
        loads.append((off, vaddr, filesz))


def va2off(v):
    for off, va, sz in loads:
        if va <= v < va + sz:
            return off + v - va


args = sys.argv[2:]
for k in range(0, len(args), 2):
    ldr, add = int(args[k], 16), int(args[k + 1], 16)
    w = struct.unpack_from('<I', data, va2off(ldr))[0]
    lit = ldr + 8 + (w & 0xFFF)
    v = struct.unpack_from('<I', data, va2off(lit))[0]
    s = (v + add + 8) & 0xFFFFFFFF
    o = va2off(s)
    print('%s/%s -> 0x%x %r' % (args[k], args[k + 1], s, data[o:data.index(b'\0', o)].decode('latin1')))
