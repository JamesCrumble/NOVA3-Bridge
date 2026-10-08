#!/bin/sh
# Which syscall seccomp killed the qemu child for (audit type=1326 lines in logcat).
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
$A logcat -d -b all 2>/dev/null | grep -aE 'type=1326|seccomp|SIGSYS' | tail -8
