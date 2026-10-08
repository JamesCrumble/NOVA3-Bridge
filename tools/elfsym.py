#!/usr/bin/env python3
"""Lists ELF32 symbols (dynsym and symtab) whose name contains a substring: elfsym.py <lib> <substr>..."""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
shoff, = struct.unpack_from('<I', data, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
secs = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
for s in secs:
    if s[1] not in (2, 11):  # SYMTAB, DYNSYM
        continue
    strtab = secs[s[6]]
    for i in range(s[5] // 16):
        name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', data, s[4] + i * 16)
        o = strtab[4] + name
        n = data[o:data.index(b'\0', o)].decode('latin1')
        if any(k in n for k in sys.argv[2:]):
            print('%s val=%#x size=%d bind=%d type=%d vis=%d shndx=%d %s' % (
                'dyn' if s[1] == 11 else 'sym', value, size, info >> 4, info & 15, other & 3, shndx, n))
