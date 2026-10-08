#!/bin/sh
# Saves all our qemu changes (against the pristine files in qemu/orig) as qemu/patches/nova3.patch
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
S=$P/qemu-src/qemu-11.1.2
O=$P/qemu/orig
out=$P/qemu/patches/nova3.patch
: > $out
for f in fpu/softfloat.c target/arm/tcg/vec_helper.c target/arm/tcg/op_helper.c linux-user/syscall.c \
         accel/tcg/tb-jmp-cache.h tcg/tcg-op.c include/tcg/tcg-op-common.h target/arm/tcg/helper-defs.h \
         target/arm/tcg/translate.c include/fpu/softfloat.h linux-user/main.c; do
  [ -f $O/$f ] || [ "$f" = fpu/softfloat.c ] || (cd $O && tar -xJf $P/downloads/qemu-11.1.2.tar.xz --strip-components=1 qemu-11.1.2/$f)
  o=$O/$f; [ "$f" = fpu/softfloat.c ] && o=$O/softfloat.c
  diff -u --label a/$f --label b/$f $o $S/$f >> $out
done
rm -f $P/qemu/patches/fastfp.patch
grep -c '^@@' $out
ls -la --time-style=+%H:%M $P/qemu/out
