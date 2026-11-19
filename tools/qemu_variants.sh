#!/bin/sh
# Builds the qemu variants compared on the phone bench, then sanity-checks each with fptest + armbench (binfmt).
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
tp() { [ -e "$P/${2:-$1}" ] && echo "$P/${2:-$1}" || echo "${SHARED_3P:-$P/../__shared-third-party__}/$1"; }  # third-party: this project first, then ../__shared-third-party__
tq() { [ -d "$P/qemu-src" ] && echo "$P/qemu-src" || echo "${SHARED_3P:-$P/../__shared-third-party__}/qemu-src/$(basename "$P")"; }  # patched qemu sources: qemu-src/ of the project, else shared
tr -d '\r' < $P/qemu/build_variant.sh > $P/qemu/build_variant_lf.sh
QS=$(tq); mkdir -p "$QS"
run() { docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v $P:/work -v "$QS":/qemu-src nova3-qemu-build bash /work/qemu/build_variant_lf.sh "$@"; }
run A "" || exit 1
run B "-DNOVA3_JMP_BITS=16" || exit 1
run C "-DNOVA3_JMP_BITS=16 -DNOVA3_FAST_LOOKUP" || exit 1
run D "-DNOVA3_JMP_BITS=16 -DNOVA3_FAST_LOOKUP -O3 -march=armv8.2-a+lse+rcpc+dotprod -mtune=neoverse-v1" || exit 1
arm-linux-gnueabihf-gcc -O1 -mcpu=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -static -o /tmp/fptest $P/qemu/fptest.c -lm
for v in A B C D; do
  echo "== $v: $($P/qemu/out/qemu-arm-$v.stripped /tmp/fptest) / $($P/qemu/out/qemu-arm-$v.stripped $P/qemu/out/armbench 1 | tail -1)"
done
