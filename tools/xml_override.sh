#!/bin/sh
# xml_override.sh <local file>  -> pushes it as xml_override.txt next to the game log;  xml_override.sh off -> removes it
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
D=/sdcard/Android/data/com.eaprules.nova3/files
if [ "$1" = off ]; then $A shell rm -f $D/xml_override.txt; echo "xml_override.txt removed"; exit 0; fi
tr -d '\r' < "$1" > /tmp/xml_override.txt
$A push /tmp/xml_override.txt $D/xml_override.txt >/dev/null && cat /tmp/xml_override.txt
