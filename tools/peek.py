#!/usr/bin/env python3
"""Prints the first words at vaddrs of an ELF32 (mapped through PT_LOAD): peek.py <lib> <hexaddr> [count]"""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<I', data, 0x1C)
phentsize, phnum = struct.unpack_from('<HH', data, 0x2A)
va = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 12
for i in range(phnum):
    t, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from('<IIIIIIII', data, phoff + i * phentsize)
    if t == 1 and vaddr <= va < vaddr + filesz:
        fo = off + va - vaddr
        for k in range(n):
            w, = struct.unpack_from('<I', data, fo + 4 * k)
            print('%08x: %08x' % (va + 4 * k, w))
        break
