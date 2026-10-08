#!/bin/sh
# grep -E "$1" over the phone's game log (last $2 lines, default 40).
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
F=/sdcard/Android/data/com.eaprules.nova3/files/nova3.log
$A shell "grep -aE '$1' $F | tail -${2:-40}"
