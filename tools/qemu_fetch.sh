#!/bin/sh
# Downloads and unpacks the qemu source, and builds the cross-build docker image.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
tp() { [ -e "$P/${2:-$1}" ] && echo "$P/${2:-$1}" || echo "${SHARED_3P:-$P/../__shared-third-party__}/$1"; }  # third-party: this project first, then ../__shared-third-party__
tq() { [ -d "$P/qemu-src" ] && echo "$P/qemu-src" || echo "${SHARED_3P:-$P/../__shared-third-party__}/qemu-src/$(basename "$P")"; }  # patched qemu sources: qemu-src/ of the project, else shared
set -e
V=${1:-11.1.2}
QS=$(tq)
mkdir -p "$(tp downloads)" "$QS"
TB=$(tp downloads)/qemu-$V.tar.xz
[ -s $TB ] || curl -sSL -o $TB https://download.qemu.org/qemu-$V.tar.xz
ls -la $TB
[ -d $QS/qemu-$V ] || tar -C $QS -xJf $TB
ls $QS/qemu-$V | head -5
docker build -q -t nova3-qemu-build $P/qemu
docker images nova3-qemu-build
