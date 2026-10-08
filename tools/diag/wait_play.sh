#!/bin/sh
# Waits (up to $1 s) for gameplay (menu=0 in the newest phase line), settles $2 s, then fps + profile.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
F=/sdcard/Android/data/com.eaprules.nova3/files/nova3.log
lim=${1:-600}; t=0
while [ $t -lt $lim ]; do
  if $A shell "grep 'phase=' $F | tail -1 | grep -q 'menu=0'"; then break; fi
  sleep 5; t=$((t+5))
done
echo "gameplay after ${t}s"
sleep ${2:-25}
sh $P/tools/run.sh $P/tools/fps.sh 0 4
sh $P/tools/run.sh $P/tools/profile.sh 10
