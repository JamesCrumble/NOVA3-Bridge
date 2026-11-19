#!/bin/sh
tp() { [ -e "$P/${2:-$1}" ] && echo "$P/${2:-$1}" || echo "${SHARED_3P:-$P/../__shared-third-party__}/$1"; }  # third-party: this project first, then ../__shared-third-party__
# Fetches qemu debug symbols once (5 MB, Debian debuginfod) and resolves the hot qemu addresses.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
DBG=$(tp downloads)/qemu-arm-static.debug
[ -s $DBG ] || curl -sL -o $DBG https://debuginfod.debian.net/buildid/9e88b55fceba2ac4091dcaa1952ed77431576689/debuginfo
ls -la $DBG
python3 -I $P/tools/qemu_hot.py $DBG $P/logs/perf_symbols.txt 45
