#!/bin/sh
# Disassembles the donor APK's classes.dex into ~/nova3probe/donor/
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
set -e
D=$P/donor
mkdir -p $D
APK=$(ls $P/port/gamebin/*.apk 2>/dev/null | head -1)
[ -n "$APK" ] || APK=$(find $P/nova3data -name '*.apk' | head -1)
echo "APK: $APK"
cd $D
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); [z.extract(n,'.') for n in z.namelist() if n.endswith('.dex')]" "$APK"
ls -la $D
$P/android-sdk/build-tools/34.0.0/dexdump -d classes.dex > classes.txt 2>/dev/null || true
wc -l classes.txt
grep -n "Class descriptor" classes.txt | grep -i gameloft | head -60
