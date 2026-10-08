#!/bin/sh
# set_gpu.sh "<renderer name>" | set_gpu.sh ""  -> writes/removes gpu_name.txt next to the game log.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
D=/sdcard/Android/data/com.eaprules.nova3/files
if [ -n "$1" ]; then
  printf '%s' "$1" > /tmp/gpu_name.txt
  $A push /tmp/gpu_name.txt "$D/gpu_name.txt" >/dev/null && echo "gpu_name.txt = $1"
else
  $A shell rm -f "$D/gpu_name.txt" && echo "gpu_name.txt removed"
fi
