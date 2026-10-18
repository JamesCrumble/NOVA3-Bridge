#!/bin/sh
# grep -E "$1" over the phone's game log (last $2 lines, default 40).
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
D=/sdcard/BridgeGames/com.jamescrumble.nova3
[ -n "$($A shell ls $D/nova3.log 2>/dev/null)" ] || D=/sdcard/Android/data/com.jamescrumble.nova3/files
F=$D/nova3.log
$A shell "grep -aE '$1' $F | tail -${2:-40}"
