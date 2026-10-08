#!/bin/sh
# Prints the last simpleperf profile left in the app's data dir, if any.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
$A shell "run-as com.eaprules.nova3 ls -la /data/data/com.eaprules.nova3/ /data/data/com.eaprules.nova3/files"
$A shell "run-as com.eaprules.nova3 sh -c 'cd /data/data/com.eaprules.nova3 && ls -la perf.data 2>&1 && simpleperf report -i perf.data --sort comm,dso 2>&1 | head -30 && simpleperf report -i perf.data --sort dso,symbol 2>&1 | head -40 | cut -c1-170'"
$A shell "cat /sdcard/Android/data/com.eaprules.nova3/files/diag_cmd.txt 2>&1 | head -5"
