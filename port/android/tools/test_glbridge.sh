#!/usr/bin/env bash
# Runs the game under qemu with the GL bridge against the Python stand-in host, on the PC.
# Usage (WSL, repo root): bash android/tools/test_glbridge.sh [SECONDS]
set -u
SECS="${1:-90}"
REPO="${REPO:-$HOME/nova3probe/nova3-native-arm}"
CMD=/tmp/glb_cmd.pipe
REP=/tmp/glb_rep.pipe
CTL=/tmp/glb_ctl
rm -f "$CMD" "$REP"; rm -rf "$CTL"; mkdir -p "$CTL"
mkfifo "$CMD" "$REP"

python3 "$REPO/android/tools/glbridge_host_test.py" "$CMD" "$REP" "$SECS" > /tmp/glb_host.log 2>&1 &
HOST=$!
sleep 1

NOVA3_CONTROL_DIR="$CTL" timeout "$((SECS + 20))" bash "$REPO/build/run_wsl.sh" "$HOME/nova3data" \
    NOVA3_GL_BRIDGE=1 NOVA3_GL_CMD="$CMD" NOVA3_GL_REPLY="$REP" \
    NOVA3_WIDTH="${WIDTH:-1280}" NOVA3_HEIGHT="${HEIGHT:-590}" NOVA3_CURSOR=0 NOVA3_FB_PROBE_INTERVAL=0 \
    SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/dev/null \
    > /tmp/glb_game.log 2>&1 &
GAME=$!

wait $HOST; HOST_RC=$?
kill $GAME 2>/dev/null; sleep 1; kill -9 $GAME 2>/dev/null
echo "=== host (rc=$HOST_RC)"; cat /tmp/glb_host.log
echo "=== game: gl bridge / viewport / errors"
grep -E 'gl bridge|GLBRIDGE|viewport scale|drawable|GL2JNILib.resize|frames=|Segmentation|fatal|abort' /tmp/glb_game.log | head -40
