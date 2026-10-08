#!/bin/sh
# Bit-exactness of the fast FP path: the same hash with and without QEMU_STRICT_FP, and with Debian's qemu 7.2 if present.
# Usage: fptest.sh [VARIANT]  (default D)
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
Q=$P/qemu/out/qemu-arm-${1:-D}.stripped
arm-linux-gnueabihf-gcc -O1 -mcpu=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -static -o /tmp/fptest $P/qemu/fptest.c -lm || exit 1
echo "fast:   $($Q /tmp/fptest)"
echo "strict: $(QEMU_STRICT_FP=1 $Q /tmp/fptest)"
D=$P/downloads/qemu-debian/qemu-arm-static
[ -x $D ] && echo "deb7.2: $($D /tmp/fptest)"
