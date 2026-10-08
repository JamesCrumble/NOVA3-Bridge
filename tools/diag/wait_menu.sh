#!/bin/sh
# Waits (up to $1 s, default 240) until the game log shows the PLAYING phase, then prints key lines.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
F=/sdcard/Android/data/com.eaprules.nova3/files/nova3.log
lim=${1:-240}; t=0
while [ $t -lt $lim ]; do
  if $A shell "grep -q 'phase=PLAYING' $F 2>/dev/null || grep -q 'qemu exited' $F 2>/dev/null"; then break; fi
  sleep 5; t=$((t+5))
done
echo "waited ${t}s"
$A shell "grep -E 'input mode|phase=|frames [0-9.]+/s|qemu exited|attaching|GLBRIDGE|FATAL|fatal' $F | tail -12"
