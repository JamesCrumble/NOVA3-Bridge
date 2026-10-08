#!/usr/bin/env bash
#
# Every tool in this directory sources this to learn which port it is working
# on. Nothing else in tools/ names a game.
#
# The single source of truth is the port's emulator.json, which the emulator harness
# already reads. Keeping the shell tools on the same file means there is one
# place to be wrong instead of two — the previous scaffold had the game's name
# typed into eight scripts, and adapting a port meant finding all eight.
#
# Usage, from the port root:
#     . portbase/tools/port_config.sh
#     echo "$PORT_NAME $ENV_PREFIX"

set -euo pipefail

PORT_ROOT="${PORT_ROOT:-$PWD}"
PORT_CONFIG="${PORT_CONFIG:-$PORT_ROOT/emulator.json}"

if [ ! -f "$PORT_CONFIG" ]; then
    echo "port_config: no emulator.json at $PORT_CONFIG" >&2
    echo "  Run from the port root, or set PORT_ROOT. Copy the file from" >&2
    echo "  portbase/template/emulator.json if this port has none yet." >&2
    exit 2
fi

# python3 rather than jq: the build image has python and adding jq to it for
# three string lookups is a dependency the harness would then also need.
_read() {
    python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" \
        "$PORT_CONFIG" "$1"
}

PORT_NAME="$(_read binary)"
ENV_PREFIX="$(_read env_prefix)"
PORT_TITLE="$(_read name)"

# The guest library, relative to the game tree root. Checking for it is how
# every tool tells "no donor" apart from "donor in an unexpected layout", and
# the difference decides whether the user or the recipe is at fault.
GUEST_SO="$(_read guest_so)"

# The build image is named after the port so two ports can be worked on at once
# without one rebuild clobbering the other's cache.
BUILD_IMAGE="${BUILD_IMAGE:-${PORT_NAME}-build}"

export PORT_ROOT PORT_CONFIG PORT_NAME ENV_PREFIX PORT_TITLE BUILD_IMAGE GUEST_SO
