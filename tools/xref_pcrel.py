#!/usr/bin/env python3
"""ARM-mode code that materialises a string's address PC-relatively (ldr rM,[pc,#i] ... add rD, pc, rM).
xref_pcrel.py <lib> <string>  -> addresses of the add instructions and the enclosing function (from .symtab)."""
import bisect
import struct
import sys

data = open(sys.argv[1], 'rb').read()
target = sys.argv[2].encode() + b'\0'
phoff, = struct.unpack_from('<I', data, 0x1C)
phentsize, phnum = struct.unpack_from('<HH', data, 0x2A)
loads = []
for i in range(phnum):
    t, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from('<IIIIIIII', data, phoff + i * phentsize)
    if t == 1:
        loads.append((off, vaddr, filesz, flags))


def off2va(o):
    for off, va, sz, fl in loads:
        if off <= o < off + sz:
            return va + o - off


# functions from .symtab
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
            funcs.append((value & ~1, size, data[o:data.index(b'\0', o)].decode('latin1')))
funcs.sort()
fstarts = [f[0] for f in funcs]

so = data.find(target)
while so > 0 and data[so - 1] != 0:
    so = data.find(target, so + 1)
sva = off2va(so)
print('string va 0x%x' % sva)
for off, va, sz, fl in loads:
    if not fl & 1:  # PF_X
        continue
    for o in range(off, off + sz - 3, 4):
        w = struct.unpack_from('<I', data, o)[0]
        if (w & 0x0FFF0FF0) != 0x008F0000 or (w >> 28) != 0xE:
            continue  # add rD, pc, rM
        rm = w & 0xF
        a = va + o - off
        for back in range(1, 12):
            p = o - 4 * back
            l = struct.unpack_from('<I', data, p)[0]
            if (l & 0xFFFFF000) == (0xE59F0000 | (rm << 12)):  # ldr rM, [pc, #imm]
                lit = (va + p - off) + 8 + (l & 0xFFF)
                lo = lit - va + off
                if off <= lo < off + sz:
                    v = struct.unpack_from('<I', data, lo)[0]
                    if (v + a + 8) & 0xFFFFFFFF == sva:
                        i = bisect.bisect_right(fstarts, a) - 1
                        fn = funcs[i][2] if i >= 0 else '?'
                        print('  add at 0x%x (rd=r%d) in %s+0x%x' % (a, (w >> 12) & 15, fn, a - funcs[i][0]))
                break
