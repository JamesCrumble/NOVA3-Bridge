#!/bin/sh
# Header bytes and zip-ness of the small .gla archives.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd $P/nova3data/saves
for f in options.gla menus.gla levels.gla strings.gla; do
  echo "== $f $(stat -c %s $f)"
  xxd -l 96 $f
  python3 -I -c "
import zipfile,sys
try:
    z=zipfile.ZipFile(sys.argv[1]); n=z.namelist(); print('ZIP', len(n)); print([x for x in n if x.lower().endswith('.xml')][:80])
except Exception as e: print('not zip:', e)
" $f
done
grep -n 'gla' $P/port/game/*.cpp | head -20
