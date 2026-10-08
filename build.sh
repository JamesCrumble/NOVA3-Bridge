#!/usr/bin/env bash
# Builds the armhf port inside ~/nova3probe. GCC 9.4 (Ubuntu 20.04) has no
# -std=gnu++20, so the Makefile's CXXFLAGS are overridden with gnu++2a.
set -eu
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
cd "$NOVA3_REPO"
make -j"$(nproc)" "$@" \
  CXXFLAGS='-std=gnu++2a -O2 -g -Wall -Wextra -Wno-unused-parameter -Werror=return-type -Winit-self -Werror=init-self -Werror=uninitialized -fno-strict-aliasing -fuse-cxa-atexit'
ls -la build/nova3
