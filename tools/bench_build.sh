#!/bin/sh
# Builds armbench (armhf static) and does a quick sanity run under the local qemu (via binfmt; timings meaningless here).
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
mkdir -p $P/qemu/out
arm-linux-gnueabihf-gcc -O2 -mcpu=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -static \
  -o $P/qemu/out/armbench $P/qemu/armbench.c -lm || exit 1
ls -la $P/qemu/out/armbench
$P/qemu/out/qemu-arm-${1:-D}.stripped $P/qemu/out/armbench 1
