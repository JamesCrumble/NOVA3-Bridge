#!/bin/sh
# Looks for the engine's device profile XMLs (GPUs.xml, GPU_*.xml, CPU_*.xml, MEM_*.xml).
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
find $P/nova3data -maxdepth 4 | head -30
echo ----
find $P/nova3data -iname 'GPU*.xml' -o -iname 'CPU_*.xml' -o -iname 'MEM_*.xml' -o -iname '*profile*' 2>/dev/null | head -30
echo ---- obb listing
cd $P/nova3data/com.gameloft.android.ANMP.GloftN3HM
for f in *.obb; do echo "== $f"; python3 -I -c "
import zipfile,sys
try:
    z=zipfile.ZipFile(sys.argv[1])
    n=[i for i in z.namelist() if any(k in i.lower() for k in ('gpu','cpu_','mem_','profile','device'))]
    print(len(z.namelist()),'entries'); print('\n'.join(n[:60]))
except Exception as e: print('not zip:',e)
" "$f"; done
grep -iE 'GPUs.xml|GPU_[0-9]|CPU_M|devices?\.(xml|cfg)|custom profile|HardwareSkinning' $P/logs/lib_strings.txt | sort -u | head -30
