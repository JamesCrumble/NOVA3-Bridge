#!/bin/sh
# Copies the phone's game log to ~/nova3probe/logs/phone.log
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A="$P/tools/adb"
D=/sdcard/BridgeGames/com.jamescrumble.nova3
[ -n "$($A shell ls $D/nova3.log 2>/dev/null)" ] || D=/sdcard/Android/data/com.jamescrumble.nova3/files
$A shell cat $D/nova3.log | tr -d '\r' > "$P/logs/phone.log"
wc -l "$P/logs/phone.log"
