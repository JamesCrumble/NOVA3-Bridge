#!/bin/sh
# Which extracted archive / OBB entry holds GPUs.xml and the GPU_n profiles.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd $P/nova3data/saves
ls
for f in *; do
  if grep -aqE 'HardwareSkinning|GPU_5|GPUs' "$f" 2>/dev/null; then echo "HIT $f $(stat -c %s "$f")"; fi
done
grep -iE 'Adreno_|custom.*profile|\.xml' $P/logs/lib_strings.txt | grep -iE 'profile|gpu|cpu|mem|custom|%s' | sort -u | head -30
