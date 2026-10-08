#!/usr/bin/env python3
"""Undefined (imported) dynamic symbols of an ELF32 and the DT_NEEDED libraries: imports.py <lib>"""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
shoff, = struct.unpack_from('<I', data, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HH', data, 0x2E)[0], struct.unpack_from('<H', data, 0x30)[0], struct.unpack_from('<H', data, 0x32)[0]
secs = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
names = []
for s in secs:
    if s[1] != 11:  # DYNSYM
        continue
    strtab = secs[s[6]]
    for i in range(s[5] // 16):
        name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', data, s[4] + i * 16)
        if shndx == 0 and name:
            o = strtab[4] + name
            names.append(data[o:data.index(b'\0', o)].decode('latin1'))
for s in secs:
    if s[1] == 6:  # DYNAMIC
        strtab = secs[s[6]]
        for i in range(s[5] // 8):
            tag, val = struct.unpack_from('<iI', data, s[4] + i * 8)
            if tag == 1:
                o = strtab[4] + val
                print('NEEDED', data[o:data.index(b'\0', o)].decode())
print(len(names), 'imports')
print(' '.join(sorted(n for n in names if not n.startswith('_Z') and not n.startswith('gl') and not n.startswith('egl'))))
