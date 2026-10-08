#!/bin/sh
# Where the qemu main thread runs: cpuset/cgroup, CPU and its frequency, cluster max freqs. Read-only.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
$A shell '
Q=$(ps -A -o PID,NAME | grep libqemu | awk "{print \$1}" | head -1)
APP=$(pidof com.eaprules.nova3)
echo "app=$APP qemu=$Q"
echo "== cpuset/cgroup app"; cat /proc/$APP/cpuset; cat /proc/$APP/cgroup | head -5
echo "== cpuset/cgroup qemu"; cat /proc/$Q/cpuset; cat /proc/$Q/cgroup | head -5
echo "== qemu threads (tid psr pcpu name)"
ps -T -p $Q -o TID,PSR,PCPU,PRI,NI,NAME | sort -k3 -nr | head -6
for c in 0 1 2 3 4 5 6 7; do
  f=/sys/devices/system/cpu/cpu$c/cpufreq
  echo "cpu$c cur=$(cat $f/scaling_cur_freq 2>/dev/null) max=$(cat $f/cpuinfo_max_freq 2>/dev/null) smax=$(cat $f/scaling_max_freq 2>/dev/null)"
done
echo "== thermal"; for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type 2>/dev/null); case "$t" in *cpu*|*skin*|*battery*|*soc*|*shell*) echo "$t $(cat $z/temp 2>/dev/null)";; esac; done | head -14
dumpsys thermalservice 2>/dev/null | grep -iE "status|throttl" | head -5
echo "== top-app cpus: $(cat /dev/cpuset/top-app/cpus 2>/dev/null) bg: $(cat /dev/cpuset/background/cpus 2>/dev/null) fg: $(cat /dev/cpuset/foreground/cpus 2>/dev/null)"
'
