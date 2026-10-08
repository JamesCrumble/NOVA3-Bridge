#!/bin/sh
# One diagnostic run with qemu -strace: push qemu_args.txt, restart, wait for exit, show the tail, clean up.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
D=/sdcard/Android/data/com.eaprules.nova3/files
printf -- '-strace\n' > /tmp/qemu_args.txt
$A push /tmp/qemu_args.txt $D/qemu_args.txt >/dev/null
$A shell am force-stop com.eaprules.nova3
$A shell am start -n com.eaprules.nova3/.MainActivity >/dev/null
t=0
while [ $t -lt 90 ]; do
  sleep 5; t=$((t+5))
  $A shell "grep -q 'qemu exited' $D/nova3.log" && break
done
echo "after ${t}s"
$A shell "tail -n 40 $D/nova3.log" | cut -c1-200
$A shell "ls -la $D/nova3.log"
$A shell rm -f $D/qemu_args.txt
