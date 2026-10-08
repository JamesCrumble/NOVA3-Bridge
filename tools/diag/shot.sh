#!/bin/sh
# Phone screenshot -> ~/nova3probe/logs/shot.png
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
$P/tools/adb exec-out screencap -p > $P/logs/shot.png
ls -la $P/logs/shot.png
