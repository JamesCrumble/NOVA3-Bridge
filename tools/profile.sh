#!/bin/sh
# simpleperf for $1 s (default 10) over the app and its qemu child; report -> ~/nova3probe/logs/perf_*.txt
# The app is profileable (not debuggable), so this runs as shell with security.perf_harden=0.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
D=${1:-10}
O=/data/local/tmp/nova3.perf
$A shell setprop security.perf_harden 0
APP=$($A shell pidof com.eaprules.nova3 | tr -d '\r')
QEMU=$($A shell "ps -A -o PID,NAME | grep libqemu | awk '{print \$1}'" | tr -d '\r' | tr '\n' ',' | sed 's/,$//')
echo "app=$APP qemu=$QEMU"
$A shell "top -b -n 1 -H -p $APP,$QEMU | head -12"
$A shell "simpleperf record --app com.eaprules.nova3 -p $APP,$QEMU --duration $D -f 2000 -o $O" 2>&1 | tail -3
$A shell "simpleperf report -i $O --sort comm,dso" | tr -d '\r' > $P/logs/perf_threads.txt
$A shell "simpleperf report -i $O --sort comm,dso,symbol" | tr -d '\r' > $P/logs/perf_symbols.txt
python3 -I $P/tools/qemu_hot.py $P/downloads/qemu-arm-static.debug $P/logs/perf_symbols.txt 30 > $P/logs/perf_qemu.txt
sed -n 1,25p $P/logs/perf_threads.txt
