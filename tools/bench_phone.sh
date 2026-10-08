#!/bin/sh
# Pushes qemu variants + armbench to /data/local/tmp/nova3bench and runs them interleaved.
# Usage: bench_phone.sh "A B C D" [passes] [taskset_mask]   -> per variant, best ms per workload.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
VARS=${1:-"A B C D"}
PASSES=${2:-3}
MASK=${3:-}
D=/data/local/tmp/nova3bench
$A shell mkdir -p $D
for v in $VARS; do
  $A push "$(wslpath -w $P/qemu/out/qemu-arm-$v.stripped)" $D/qemu-$v >/dev/null || exit 1
done
$A push "$(wslpath -w $P/qemu/out/armbench)" $D/armbench >/dev/null || exit 1
$A shell chmod 755 $D/qemu-* $D/armbench
OUT=$P/logs/bench_$(date +%H%M%S).txt
: > $OUT
pre=""; [ -n "$MASK" ] && pre="taskset $MASK"
for pass in $(seq 1 $PASSES); do
  for v in $VARS; do
    $A shell "cd $D && $pre ./qemu-$v ./armbench 1" | tr -d '\r' | sed "s/^/$v /" >> $OUT
  done
done
python3 - "$OUT" <<'EOF'
import sys, collections
best = collections.defaultdict(dict)
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) >= 3:
        v, w, ms = p[0], p[1], float(p[2])
        best[v][w] = min(ms, best[v].get(w, 1e30))
ws = ['calls', 'vfp', 'neon', 'ints', 'mem', 'total']
print('var ' + ''.join('%9s' % w for w in ws))
base = None
for v in best:
    print('%-3s ' % v + ''.join('%9.1f' % best[v].get(w, 0) for w in ws))
    if base is None: base = best[v]
print('speedup vs first:')
for v in best:
    print('%-3s ' % v + ''.join('%8.2fx' % (base.get(w, 0) / best[v][w] if best[v].get(w) else 0) for w in ws))
EOF
echo "raw: $OUT"
