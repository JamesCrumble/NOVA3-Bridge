#!/bin/sh
# Contents of the region's Tango extension marker and its neighbours. Read-only.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
$A shell '
ls -la /my_region/global/etc/extension/ | head -40
echo "== 1554_Tango"; ls -la /my_region/global/etc/extension/1554_Tango; cat /my_region/global/etc/extension/1554_Tango 2>/dev/null | head -c 2000; echo
find /my_region/global/etc/extension/1554_Tango -maxdepth 3 2>/dev/null | head -30
'
