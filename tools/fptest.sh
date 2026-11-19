#!/bin/sh
# Bit-exactness of the fast FP path: the same hash with and without QEMU_STRICT_FP, and with Debian's qemu 7.2 if present.
# Usage: fptest.sh [VARIANT]  (default D)
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
tp() { [ -e "$P/${2:-$1}" ] && echo "$P/${2:-$1}" || echo "${SHARED_3P:-$P/../__shared-third-party__}/$1"; }  # third-party: this project first, then ../__shared-third-party__
tq() { [ -d "$P/qemu-src" ] && echo "$P/qemu-src" || echo "${SHARED_3P:-$P/../__shared-third-party__}/qemu-src/$(basename "$P")"; }  # patched qemu sources: qemu-src/ of the project, else shared
Q=$P/qemu/out/qemu-arm-${1:-D}.stripped
arm-linux-gnueabihf-gcc -O1 -mcpu=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -static -o /tmp/fptest $P/qemu/fptest.c -lm || exit 1
echo "fast:   $($Q /tmp/fptest)"
echo "strict: $(QEMU_STRICT_FP=1 $Q /tmp/fptest)"
D=$(tp downloads)/qemu-debian/qemu-arm-static
[ -x $D ] && echo "deb7.2: $($D /tmp/fptest)"
