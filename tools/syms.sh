#!/bin/sh
# syms.sh <substr>... : game library symbols containing any substring (name, value, size), TinyXML filtered out.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
python3 -I $P/tools/elfsym.py $P/nova3data/lib/armeabi-v7a/libNOVA3_neon.so "$@" \
  | grep -v TiXml | awk '{print $NF, $2, $3}' | sort -u | head -${N:-80}
