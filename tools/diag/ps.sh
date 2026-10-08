#!/bin/sh
# Shows the wrapper's processes and threads on the phone.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
$A shell "ps -A -o PID,PPID,STAT,%CPU,RSS,TIME,NAME | grep -E 'nova3|qemu|PID'"
$A shell "dumpsys activity activities | grep -E 'com.eaprules.nova3' | head -20"
