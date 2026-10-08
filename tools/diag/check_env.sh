#!/bin/sh
# What the WSL box offers for cross-building qemu (no installs).
echo "== docker"; (docker version --format '{{.Server.Version}}' 2>&1 | head -2) || true
echo "== python"; python3 --version; python3 -c "import venv, ensurepip; print('venv ok')" 2>&1 | tail -1
echo "== tools"; for t in meson ninja pkg-config gcc make bison flex git xz zstd curl; do printf '%s: ' $t; command -v $t || echo missing; done
echo "== cross"; ls /usr/bin | grep -E 'aarch64.*gcc|clang' | head
echo "== disk"; df -h /home | tail -1
echo "== cpu"; nproc
