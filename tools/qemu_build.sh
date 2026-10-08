#!/bin/sh
# Builds our qemu-arm in Docker and checks float exactness.
# Usage: qemu_build.sh [VARIANT [EXTRA_CFLAGS]]   (default D: the build shipped in the APK)
# Needs: qemu/Dockerfile built as image nova3-qemu-build, qemu source patched in qemu-src/ (tools/qemu_fetch.sh).
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
V=${1:-D}
case "$V" in
  D) CF=${2:-"-DNOVA3_JMP_BITS=16 -DNOVA3_FAST_LOOKUP -O3 -march=armv8.2-a+lse+rcpc+dotprod -mtune=neoverse-v1"} ;;
  *) CF=${2:-} ;;
esac
tr -d '\r' < $P/qemu/build_variant.sh > $P/qemu/.build_variant_lf.sh
docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v $P:/work nova3-qemu-build \
  bash /work/qemu/.build_variant_lf.sh "$V" "$CF" || exit 1
sh $P/tools/fptest.sh "$V"
