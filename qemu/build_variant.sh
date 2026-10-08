#!/bin/bash
# Inside the nova3-qemu-build container (~/nova3probe at /work): one qemu-arm variant per build dir.
# Usage: build_variant.sh NAME "EXTRA_CFLAGS"   -> /work/qemu/out/qemu-arm-NAME(.stripped)
set -euo pipefail
NAME=$1
CF=${2:-}
V=${V:-11.1.2}
SRC=/work/qemu-src/qemu-$V
B=/work/qemu/obj-$NAME
mkdir -p "$B" /work/qemu/out
cd "$B"
if [ ! -f build.ninja ]; then
  PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
  "$SRC/configure" \
    --target-list=arm-linux-user --static \
    --disable-system --disable-tools --disable-docs --disable-guest-agent \
    --disable-qom-cast-debug --disable-debug-tcg --disable-plugins \
    --disable-werror --disable-rust \
    --cross-prefix=aarch64-linux-gnu- \
    --extra-cflags="-O2 $CF" \
    > /work/qemu/configure-$NAME.log 2>&1 || { tail -30 /work/qemu/configure-$NAME.log; exit 1; }
fi
ninja qemu-arm > /work/qemu/ninja-$NAME.log 2>&1 || { grep -E 'error' /work/qemu/ninja-$NAME.log | head -20; exit 1; }
cp qemu-arm /work/qemu/out/qemu-arm-$NAME
aarch64-linux-gnu-strip -o /work/qemu/out/qemu-arm-$NAME.stripped qemu-arm
ls -la /work/qemu/out/qemu-arm-$NAME.stripped
