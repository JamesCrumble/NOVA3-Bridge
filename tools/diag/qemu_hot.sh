#!/bin/sh
# Fetches qemu debug symbols once (5 MB, Debian debuginfod) and resolves the hot qemu addresses.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
DBG=$P/downloads/qemu-arm-static.debug
[ -s $DBG ] || curl -sL -o $DBG https://debuginfod.debian.net/buildid/9e88b55fceba2ac4091dcaa1952ed77431576689/debuginfo
ls -la $DBG
python3 -I $P/tools/qemu_hot.py $DBG $P/logs/perf_symbols.txt 45
