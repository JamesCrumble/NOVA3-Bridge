#!/bin/sh
# xmldump.sh on   -> creates xmldump.flag next to the log
# xmldump.sh pull -> copies the dumped XML to ~/nova3probe/logs/xmldump and lists profile-looking ones
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
D=/sdcard/Android/data/com.eaprules.nova3/files
case "$1" in
on)  : > /tmp/xmldump.flag; $A push /tmp/xmldump.flag $D/xmldump.flag >/dev/null && echo "xmldump on" ;;
off) $A shell rm -f $D/xmldump.flag && echo "xmldump off" ;;
pull)
  rm -rf $P/logs/xmldump; mkdir -p $P/logs/xmldump
  $A pull $D/xmldump/. "$(wslpath -w $P/logs/xmldump)" | tail -1
  ls $P/logs/xmldump | wc -l
  grep -l -iE 'HardwareSkinning|Geometry level|GPU_[0-9]' $P/logs/xmldump/*.xml | head -40 ;;
esac
