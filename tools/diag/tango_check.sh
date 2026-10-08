#!/bin/sh
# Where Tango lives and whether a 32-bit zygote runs. Read-only.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
$A shell '
echo "== props"; getprop | grep -iE "tango|zygote|abilist|bridge|32bit|arm32" | head -30
echo "== processes"; ps -A -o PID,NAME,ARGS 2>/dev/null | grep -iE "zygote|tango" | grep -v grep | head
echo "== files"
for d in /system /system_ext /product /vendor /odm /my_product /my_engineering /my_heytap /my_stock /my_region /my_bigball /my_carrier /my_company /my_preload /apex; do
  ls -d $d >/dev/null 2>&1 || continue
  find $d -iname "*tango*" 2>/dev/null | head -20
done
echo "== nova packages"; pm list packages | grep -iE "nova|gameloft"
echo "== system/lib"; ls -la /system/lib 2>/dev/null | head -8
'
