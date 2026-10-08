#!/bin/sh
# engine_env.sh "KEY=VALUE" ...  -> writes engine_env.txt next to the game log (no args: removes it)
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
D=/sdcard/Android/data/com.eaprules.nova3/files
if [ $# -eq 0 ]; then $A shell rm -f $D/engine_env.txt; echo "engine_env.txt removed"; exit 0; fi
: > /tmp/engine_env.txt
for kv in "$@"; do echo "$kv" >> /tmp/engine_env.txt; done
$A push /tmp/engine_env.txt $D/engine_env.txt >/dev/null && cat /tmp/engine_env.txt
