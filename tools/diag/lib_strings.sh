#!/bin/sh
# Engine strings about skinning, quality profiles and device detection in libNOVA3_neon.so.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
L=$P/nova3data/lib/armeabi-v7a/libNOVA3_neon.so
strings -n 5 $L > $P/logs/lib_strings.txt
wc -l $P/logs/lib_strings.txt
grep -iE 'skin' $P/logs/lib_strings.txt | sort -u | head -40
echo ----
grep -iE 'quality|lowend|low_end|highend|hd_|profile|adreno|tegra|mali|powervr|sgx|GL_RENDERER' $P/logs/lib_strings.txt | sort -u | head -60
