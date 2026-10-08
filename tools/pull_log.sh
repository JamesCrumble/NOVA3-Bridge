#!/bin/sh
# Copies the phone's game log to ~/nova3probe/logs/phone.log
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
"$P/tools/adb" shell cat /sdcard/Android/data/com.eaprules.nova3/files/nova3.log | tr -d '\r' > "$P/logs/phone.log"
wc -l "$P/logs/phone.log"
