#!/bin/sh
# Downloads and unpacks the qemu source, and builds the cross-build docker image.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
set -e
V=${1:-11.1.2}
mkdir -p $P/downloads $P/qemu-src
T=$P/downloads/qemu-$V.tar.xz
[ -s $T ] || curl -sSL -o $T https://download.qemu.org/qemu-$V.tar.xz
ls -la $T
[ -d $P/qemu-src/qemu-$V ] || tar -C $P/qemu-src -xJf $T
ls $P/qemu-src/qemu-$V | head -5
docker build -q -t nova3-qemu-build $P/qemu
docker images nova3-qemu-build
