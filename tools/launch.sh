#!/bin/sh
# Installs the built APK (if "install" given), force-stops and starts the game.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
if [ "${1:-}" = install ]; then
  $A install -r "$(wslpath -w $P/port/NOVA3-ARM32-debug.apk)" || exit 1
fi
$A shell am force-stop com.eaprules.nova3
$A shell am start -n com.eaprules.nova3/.MainActivity
