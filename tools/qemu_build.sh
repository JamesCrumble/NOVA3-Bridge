#!/bin/sh
# Builds our qemu-arm in Docker and checks float exactness.
# Usage: qemu_build.sh [VARIANT [EXTRA_CFLAGS]]   (default D: the build shipped in the APK)
# Needs: qemu/Dockerfile built as image nova3-qemu-build, qemu source patched in ../__shared-third-party__/qemu-src/<project>/ (tools/qemu_fetch.sh).
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
tp() { [ -e "$P/${2:-$1}" ] && echo "$P/${2:-$1}" || echo "${SHARED_3P:-$P/../__shared-third-party__}/$1"; }  # third-party: this project first, then ../__shared-third-party__
tq() { [ -d "$P/qemu-src" ] && echo "$P/qemu-src" || echo "${SHARED_3P:-$P/../__shared-third-party__}/qemu-src/$(basename "$P")"; }  # patched qemu sources: qemu-src/ of the project, else shared
V=${1:-D}
case "$V" in
  D) CF=${2:-"-DNOVA3_JMP_BITS=16 -DNOVA3_FAST_LOOKUP -O3 -march=armv8.2-a+lse+rcpc+dotprod -mtune=neoverse-v1"} ;;
  *) CF=${2:-} ;;
esac
tr -d '\r' < $P/qemu/build_variant.sh > $P/qemu/.build_variant_lf.sh
QS=$(tq); mkdir -p "$QS"
docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v $P:/work -v "$QS":/qemu-src nova3-qemu-build \
  bash /work/qemu/.build_variant_lf.sh "$V" "$CF" || exit 1
sh $P/tools/fptest.sh "$V"
