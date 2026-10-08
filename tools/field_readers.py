#!/usr/bin/env python3
"""ARM code that calls a getter (bl <getter>) and soon reads a byte/word field of the result.
field_readers.py <lib> <getter_hex> <offset_dec> [window]"""
import bisect
import struct
import sys

data = open(sys.argv[1], 'rb').read()
getter = int(sys.argv[2], 16)
field = int(sys.argv[3])
win = int(sys.argv[4]) if len(sys.argv) > 4 else 16
phoff, = struct.unpack_from('<I', data, 0x1C)
phentsize, phnum = struct.unpack_from('<HH', data, 0x2A)
loads = []
for i in range(phnum):
    t, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from('<IIIIIIII', data, phoff + i * phentsize)
    if t == 1:
        loads.append((off, vaddr, filesz, flags))
shoff, = struct.unpack_from('<I', data, 0x20)
shentsize, shnum = struct.unpack_from('<HH', data, 0x2E)
secs = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
funcs = []
for s in secs:
    if s[1] != 2:
        continue
    st = secs[s[6]]
    for i in range(s[5] // 16):
        name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', data, s[4] + i * 16)
        if info & 15 == 2 and value:
            o = st[4] + name
            funcs.append((value & ~1, data[o:data.index(b'\0', o)].decode('latin1')))
funcs.sort()
fs = [f[0] for f in funcs]
for off, va, sz, fl in loads:
    if not fl & 1:
        continue
    for o in range(off, off + sz - 3, 4):
        w = struct.unpack_from('<I', data, o)[0]
        if (w & 0x0F000000) != 0x0B000000 or (w >> 28) != 0xE:
            continue  # bl
        a = va + o - off
        imm = w & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        if a + 8 + imm * 4 != getter:
            continue
        for k in range(1, win):
            x = struct.unpack_from('<I', data, o + 4 * k)[0]
            # ldrb/ldr rT, [r0, #field] (immediate offset, pre-indexed, no writeback)
            if (x & 0x0F7F0FFF) == (0x05500000 | field) or (x & 0x0F7F0FFF) == (0x05100000 | field):
                i = bisect.bisect_right(fs, a) - 1
                print('0x%x %s+0x%x -> %s at +%d' % (a, funcs[i][1][:90], a - funcs[i][0],
                      'ldrb' if x & 0x00400000 else 'ldr', k))
                break
