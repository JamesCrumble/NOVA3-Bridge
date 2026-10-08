#!/bin/sh
# What the hot qemu addresses are: symbol table presence and nearby strings.
Q=$P/downloads/qemu-debian/qemu-arm-static
file $Q
aarch64-linux-gnu-nm $Q 2>&1 | head -3 || true
which aarch64-linux-gnu-objdump objdump gdb-multiarch 2>/dev/null
ls $P/downloads/qemu-debian 2>/dev/null | head
dpkg -l | grep -E 'qemu|binutils-aarch64' | awk '{print $2, $3}'
