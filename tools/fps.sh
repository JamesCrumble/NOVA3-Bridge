#!/bin/sh
# Waits $1 s (default 0), then prints the last $2 (default 4) bridge stat lines and phase lines.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
F=/sdcard/Android/data/com.eaprules.nova3/files/nova3.log
[ "${1:-0}" -gt 0 ] && sleep "$1"
$A shell "grep -E 'frames [0-9.]+/s|phase=|qemu exited|died' $F | tail -${2:-4}"
