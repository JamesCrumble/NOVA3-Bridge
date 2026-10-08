#!/bin/sh
# Waits $1 s (default 0), then: screenshot + recent fps/touch lines from the game log.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
F=/sdcard/Android/data/com.eaprules.nova3/files/nova3.log
[ "${1:-0}" -gt 0 ] && sleep "$1"
$A exec-out screencap -p > $P/logs/shot.png
$A shell "grep -E 'phase=|frames [0-9.]+/s|input: touch|input mode|qemu exited' $F | tail -${2:-14}"
