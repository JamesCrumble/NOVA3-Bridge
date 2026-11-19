#!/bin/sh
tp() { [ -e "$P/${2:-$1}" ] && echo "$P/${2:-$1}" || echo "${SHARED_3P:-$P/../__shared-third-party__}/$1"; }  # third-party: this project first, then ../__shared-third-party__
# What the hot qemu addresses are: symbol table presence and nearby strings.
Q=$(tp downloads)/qemu-debian/qemu-arm-static
file $Q
aarch64-linux-gnu-nm $Q 2>&1 | head -3 || true
which aarch64-linux-gnu-objdump objdump gdb-multiarch 2>/dev/null
ls $(tp downloads)/qemu-debian 2>/dev/null | head
dpkg -l | grep -E 'qemu|binutils-aarch64' | awk '{print $2, $3}'
