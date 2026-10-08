#!/usr/bin/env bash
# Source this from the repository root: `source env.sh`. Touches only the current shell.
_P="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
export NOVA3_PROBE="$_P"
export NOVA3_REPO="$_P/port"
export NOVA3_GAMEDIR="$_P/nova3data"
export JAVA_HOME="$_P/jdk"
export ANDROID_HOME="$_P/android-sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export PKG_CONFIG_PATH="$_P/sdl-arm/lib/pkgconfig"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/build-tools/34.0.0:$ANDROID_HOME/cmdline-tools/latest/bin:$_P/downloads/qemu-debian:$PATH"
unset _P
