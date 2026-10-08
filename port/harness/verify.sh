#!/usr/bin/env bash
#
# Autonomous verification harness for the N.O.V.A. 3 port.
#
# It builds the armhf loader, runs it under qemu-arm with software GLES, and
# decides objectively which milestone the port currently reaches. No console,
# no SD card, no eyeballs.
#
# Exit code = highest milestone passed. The loop reads it to know whether the
# last iteration moved forward, stalled, or regressed.
#
# ---------------------------------------------------------------------------
# What makes this N.O.V.A. 3's harness and not a copy of a sibling's
#
# The engine is the same Gameloft GLES2 runtime Modern Combat 3 uses, so the
# shape of the boot is shared - but every name below is this game's, and each
# one differs from the sibling in a way that would pin the loop at a milestone
# forever if it were left as inherited:
#
#   - The package is GloftN3HM and the JNI class is
#     com/gameloft/android/ANMP/GloftN3HM/GL2JNILib. MC3's GL2JNILib lives under
#     com/gameloft/glf/ and its activity class (GloftM3HM.Init) does not exist
#     here at all: this build has no per-game Init export, the whole boot is
#     GL2JNILib.
#   - There is no isGamePlay. The engine's own notion of progress is
#     nativeIsMainMenuOrIGM, which answers the *opposite* question - true means
#     "in a menu" - so M7 reads it inverted.
#   - Both expansion files are ordinary zips (MC3's main.obb is a Gameloft
#     container addressed by hash), and they sit in the donor under the package
#     directory, which is where Android's /sdcard/Android/obb/<pkg> maps to.
#   - readelf says NEEDED libOpenSLES and no libz: audio is OpenSL ES, and
#     nothing here needs portbase's zlib thunks to boot.
#
# A harness copied without that adjustment does not fail loudly. It fails
# quietly, and the loop burns iterations satisfying a check about another game.
# ---------------------------------------------------------------------------
#
# Usage:  harness/verify.sh [--timeout SECS]
set -uo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="mc3-build"

# The game is the player's own copy and never ships with the port: the
# extracted tree, with lib/armeabi-v7a/ and the package directory holding the
# two .obb expansion files.
GAMEDIR="${NOVA3_GAMEDIR:-$HOME/Archive/handheld/donors/nova3}"

PKG="com.gameloft.android.ANMP.GloftN3HM"

# 300s, matching the sibling port. Every frame is rasterised by llvmpipe in
# software under qemu and the run settles well under one frame per second; the
# frame budget is what M5 is about, so the clock gives way to it.
TIMEOUT="${TIMEOUT:-300}"
RESULTS="$PORT_DIR/harness/results"
mkdir -p "$RESULTS"

log()  { echo "[verify] $*"; }
fail() { echo "[verify] FAIL: $*"; }

# Milestones, in order. Each one is a hard, observable fact - not an opinion.
#   0 control   qemu-arm in this image runs an armhf binary at all
#   1 build     the loader links into an armhf binary
#   2 load      libNOVA3_neon.so maps with every import resolved
#   3 jni       JNI_OnLoad, GL2JNILib.init and GL2JNILib.initGL all return
#   4 gl        a live GLES 2.0 context, and the engine took the resize
#   5 frames    at least MIN_FRAMES GL2JNILib.step calls, presented
#   6 content   the engine loaded its own data and is drawing it
#   7 autopilot the game advances when fed input, it is not parked in a menu
MIN_FRAMES="${MIN_FRAMES:-60}"

REACHED=0

# ------------------------------------------------------- 0. positive control
#
# Before anything else: prove that this image can run a 32-bit ARM binary.
#
# Without it, every failure below has two explanations - the port is broken, or
# qemu/the cross libraries in the image are - and they are indistinguishable
# from a log. The control is a five-line program built by the same compiler and
# run by the same qemu invocation as the loader, so it fails for exactly the
# reasons that are not the port's fault.
log "M0 positive control (qemu-arm runs an armhf binary)"
if docker run --rm "$IMAGE" sh -c '
      printf "#include <cstdio>\nint main(){printf(\"control ok\\\\n\");}" > /tmp/c.cpp &&
      arm-linux-gnueabihf-g++ -O0 -o /tmp/c /tmp/c.cpp &&
      qemu-arm -L /usr/arm-linux-gnueabihf /tmp/c' 2>&1 | grep -q "control ok"; then
    REACHED=0
    log "M0 ok"
else
    fail "M0: qemu-arm cannot run an armhf binary in $IMAGE - nothing below this"
    fail "    line would be attributable to the port. Rebuild the image."
    echo "0" > "$RESULTS/milestone"
    exit 0
fi

# ---------------------------------------------------------------- 1. build
log "M1 build"
if docker run --rm -v "$PORT_DIR":/src -w /src "$IMAGE" \
     make -j"$(nproc 2>/dev/null || echo 4)" \
     > "$RESULTS/01-build.log" 2>&1; then
    if docker run --rm -v "$PORT_DIR":/src -w /src "$IMAGE" \
         file build/nova3 2>/dev/null | grep -q "ELF 32-bit.*ARM"; then
        REACHED=1
        log "M1 ok"
    else
        fail "build produced no armhf binary"
    fi
else
    fail "compile error, see 01-build.log"
    tail -20 "$RESULTS/01-build.log"
    echo "$REACHED" > "$RESULTS/milestone"
    exit $REACHED
fi

if [ ! -d "$GAMEDIR" ]; then
    fail "game directory not found at $GAMEDIR (set NOVA3_GAMEDIR)"
    echo "$REACHED" > "$RESULTS/milestone"
    exit $REACHED
fi

# The sha1 of the build this port is written against. The donor also ships
# libNOVA3_8_neon.so, a different variant of the same game with its own export
# addresses; picking it up by accident would produce faults that say nothing
# about why.
SO_FILE="$GAMEDIR/lib/armeabi-v7a/libNOVA3_neon.so"
EXPECT_SHA1="ae97a4423fc6b9d003cc534fd4a4d03e6d0df0c0"
if [ -f "$SO_FILE" ]; then
    GOT_SHA1=$(shasum -a 1 "$SO_FILE" 2>/dev/null | cut -d' ' -f1 | tr 'A-Z' 'a-z')
    if [ "$GOT_SHA1" != "$EXPECT_SHA1" ]; then
        fail "wrong game build: sha1 $GOT_SHA1, expected $EXPECT_SHA1 (N.O.V.A. 3 1.0.7)"
        echo "$REACHED" > "$RESULTS/milestone"
        exit $REACHED
    fi
else
    fail "no $SO_FILE - this does not look like an extracted N.O.V.A. 3 tree"
    echo "$REACHED" > "$RESULTS/milestone"
    exit $REACHED
fi

# The expansion files, by their real names. Unlike the sibling's main container
# these are both ordinary zips, so a truncated download is worth catching here
# rather than as an engine fault two milestones later.
OBB_MAIN_NAME="main.1050.$PKG.obb"
OBB_PATCH_NAME="patch.1070.$PKG.obb"
for f in "$OBB_MAIN_NAME" "$OBB_PATCH_NAME"; do
    if [ ! -f "$GAMEDIR/$PKG/$f" ]; then
        fail "no $GAMEDIR/$PKG/$f - the expansion files belong in the package"
        fail "    directory, which is where Android's /sdcard/Android/obb/<pkg> maps to"
        echo "$REACHED" > "$RESULTS/milestone"
        exit $REACHED
    fi
done

# ------------------------------------------------- 2-7. run under emulation
log "M2-M7 running under qemu-arm (timeout ${TIMEOUT}s)"

docker run --rm \
    -v "$PORT_DIR":/src \
    -v "$GAMEDIR":/game:ro \
    -w /src \
    -e SDL_VIDEODRIVER=offscreen \
    -e LIBGL_ALWAYS_SOFTWARE=1 \
    -e GALLIUM_DRIVER=llvmpipe \
    -e EGL_PLATFORM=surfaceless \
    -e LOADER_TRACE=1 \
    -e NOVA3_FRAME_LIMIT="$((MIN_FRAMES * 10))" \
    "$IMAGE" \
    timeout --signal=INT "$TIMEOUT" \
    qemu-arm -L /usr/arm-linux-gnueabihf \
        ./build/nova3 /game \
    > "$RESULTS/02-run.log" 2>&1
RUN_RC=$?

RUN_LOG="$RESULTS/02-run.log"

# --- M2: every import resolved. This game declares 336 of them, 100 of which
#         are gl*. An unresolved symbol means a missing thunk, which is the
#         most common failure mode for this kind of port.
if grep -q "TRACE: module loaded" "$RUN_LOG"; then
    UNRESOLVED=$(grep -c "unresolved symbol" "$RUN_LOG" || true)
    if [ "$UNRESOLVED" -eq 0 ]; then
        REACHED=2; log "M2 ok (0 unresolved symbols)"
    else
        fail "M2: $UNRESOLVED unresolved symbols"
        grep "unresolved symbol" "$RUN_LOG" | sort -u | head -20
    fi
else
    fail "M2: module never loaded"
    grep -iE "fatal|cannot|no such" "$RUN_LOG" | head -10
fi

# --- M3: the JNI handshake. Each of these has to *return*, not merely be
#         called: JNI_OnLoad and the two inits walk the fake class registry,
#         and a missing class shows up as a fault inside it, never as a
#         diagnostic naming the class.
#
# There is no per-game activity Init here. On Android the whole boot is
# GL2JNILib: the activity calls init(), and the GL thread calls initGL() and
# InitViewSettings() from inside its EGL config chooser. init() is also where
# the engine calls back into Java for setupPaths(), so a port whose fake
# GL2JNILib is missing that method gets past init() with no paths set and
# fails at M6 with every file missing - which is why setPaths is asserted here
# and not left to be inferred from the failures it causes.
if [ $REACHED -ge 2 ]; then
    OK=1
    for step in "JNI_OnLoad returned" \
                "GL2JNILib.init returned" \
                "GL2JNILib.initGL returned" \
                "GL2JNILib.setPaths returned"; do
        grep -q "TRACE: $step" "$RUN_LOG" || { fail "M3: '$step' never printed"; OK=0; }
    done
    if [ $OK -eq 1 ]; then
        REACHED=3; log "M3 ok"
    else
        grep -iE "FindClass|does not have|no class|no method|no field" "$RUN_LOG" \
            | sort -u | head -10
    fi
fi

# --- M4: a live GLES 2.0 context the engine accepted.
#
# Not GLES 1.1. This game imports 100 gl* entry points and every one of them is
# a shader-pipeline call; a context without them resolves glCreateShader to
# null and the first step() jumps to address 0.
if [ $REACHED -ge 3 ]; then
    if grep -q "TRACE: GL_VERSION=" "$RUN_LOG" \
       && grep -q "TRACE: GL2JNILib.resize" "$RUN_LOG"; then
        REACHED=4; log "M4 ok ($(grep -oE 'TRACE: GL_VERSION=.*' "$RUN_LOG" | tail -1))"
    else
        fail "M4: no GLES 2.0 context, or the engine never took the resize"
        grep -iE "GL_VERSION|GL_RENDERER|egl|SDL_CreateWindow|context" "$RUN_LOG" | head -10
    fi
fi

# --- M5: it is presenting. The loader owns this loop, so a frame count stuck
#         at zero means our driver never called in, not that the engine
#         stalled - worth telling apart in the log.
FRAMES=$(grep -oE "TRACE: frames=[0-9]+" "$RUN_LOG" | tail -1 | cut -d= -f2)
FRAMES="${FRAMES:-0}"
if [ $REACHED -ge 4 ] && [ "$FRAMES" -ge "$MIN_FRAMES" ]; then
    REACHED=5; log "M5 ok ($FRAMES frames)"
elif [ $REACHED -ge 4 ]; then
    fail "M5: only $FRAMES frames (need $MIN_FRAMES)"
    grep -iE "segfault|abort|fatal|signal|SIGSEGV" "$RUN_LOG" | head -10
fi

# --- M6: the engine is running its own content, not just holding a surface.
#
# A solid-colour clear is indistinguishable from a rendered game by a pixel
# test alone, which is how a sibling port once passed while opening zero
# assets. What this game's data looks like is: both expansion files opened,
# textures uploaded out of them, and draw calls issued.
#
# portbase's assets counter increments on paths containing
# "/assets/published/", a different sibling's layout. It is structurally zero
# for a game that reads everything out of two containers, so it is printed for
# the record and never asserted on.
MIN_TEXTURES="${MIN_TEXTURES:-10}"
MIN_DRAWS="${MIN_DRAWS:-100}"

if [ $REACHED -ge 5 ]; then
    NONBLACK=$(grep -c "TRACE: framebuffer non-black" "$RUN_LOG" || true)
    SUMMARY=$(grep -oE "TRACE: summary assets=[0-9]+ textures=[0-9]+ draws=[0-9]+" "$RUN_LOG" | tail -1)

    if [ -z "$SUMMARY" ]; then
        fail "M6: loader emitted no summary line"
    else
        A=$(echo "$SUMMARY" | grep -oE "assets=[0-9]+"   | cut -d= -f2)
        T=$(echo "$SUMMARY" | grep -oE "textures=[0-9]+" | cut -d= -f2)
        D=$(echo "$SUMMARY" | grep -oE "draws=[0-9]+"    | cut -d= -f2)

        # Both expansion containers, opened - BY THE LOADER, not by the game.
        # This engine never reads its .obb files: Android's installer used to
        # unpack them and the engine opens flat files, so the port unpacks on
        # demand (game/obb_cache.cpp) and the guest's io trace will never show
        # the containers. The original assert here watched that io trace - a
        # sibling's model - and no correct implementation could satisfy it.
        # The equivalent evidence is the cache's own trace: each container
        # opened by name, plus at least one entry actually served out of them,
        # which is what proves content flows and not merely that two zips
        # parse.
        OBB_MAIN=$(grep -c "obb: opened main\.1050\..*\.obb"  "$RUN_LOG" || true)
        OBB_PATCH=$(grep -c "obb: opened patch\.1070\..*\.obb" "$RUN_LOG" || true)
        OBB_SERVED=$(grep -c "obb: extracted " "$RUN_LOG" || true)

        log "M6 counters: obb main=$OBB_MAIN patch=$OBB_PATCH served=$OBB_SERVED textures=$T draws=$D nonblack=$NONBLACK (portbase assets counter=$A, inert for this game)"

        # 124 == the timeout killed it, which is success for a game loop.
        ALIVE=0
        { [ $RUN_RC -eq 124 ] || [ $RUN_RC -eq 0 ]; } && ALIVE=1

        if [ $ALIVE -eq 1 ] && [ "$NONBLACK" -gt 0 ] \
           && [ "$OBB_MAIN" -gt 0 ] && [ "$OBB_PATCH" -gt 0 ] \
           && [ "$OBB_SERVED" -gt 0 ] \
           && [ "$T" -ge "$MIN_TEXTURES" ] \
           && [ "$D" -ge "$MIN_DRAWS" ]; then
            REACHED=6
            log "M6 ok (both .obb opened, textures=$T draws=$D, survived)"
        else
            [ $ALIVE -eq 0 ]             && { fail "M6: exited early rc=$RUN_RC"; grep -iE "segfault|abort|fatal|signal" "$RUN_LOG" | head -10; }
            [ "$NONBLACK" -eq 0 ]        && fail "M6: renders only black frames"
            [ "$OBB_MAIN" -eq 0 ]        && fail "M6: $OBB_MAIN_NAME never opened by the obb cache - check nova3_obb_dir() and game/obb_cache.cpp"
            [ "$OBB_PATCH" -eq 0 ]       && fail "M6: $OBB_PATCH_NAME never opened by the obb cache - same place"
            [ "$OBB_SERVED" -eq 0 ]      && fail "M6: containers opened but no entry was ever extracted - the engine asked for nothing, or the misses never reached the cache"
            [ "$T" -lt "$MIN_TEXTURES" ] && fail "M6: only $T textures uploaded (need $MIN_TEXTURES)"
            [ "$D" -lt "$MIN_DRAWS" ]    && fail "M6: only $D draw calls (need $MIN_DRAWS)"
            grep -iE "missing|no class|not found|ENOENT" "$RUN_LOG" | sort -u | head -8
        fi
    fi
fi

# --- M7: it *advances*, not just draws.
#
# A game parked in its main menu waiting for someone to press A renders at full
# speed, opens its assets and passes M6 whole. This game answers the question
# itself - nativeIsMainMenuOrIGM is an export - but note the polarity: it is
# true while the engine is in the main menu or the in-game menu, so leaving
# those is mainmenu going 1 -> 0, the reverse of the sibling's isGamePlay.
# The framebuffer hash is the second witness, and the one that works even if
# the export lies.
MIN_SCENES="${MIN_SCENES:-2}"

if [ $REACHED -ge 6 ]; then
    AUTO=$(grep -oE "TRACE: autopilot keys=[0-9]+ scenes=[0-9]+" "$RUN_LOG" | tail -1)
    if [ -z "$AUTO" ]; then
        fail "M7: loader emitted no autopilot line"
    else
        K=$(echo "$AUTO" | grep -oE "keys=[0-9]+"   | cut -d= -f2)
        S=$(echo "$AUTO" | grep -oE "scenes=[0-9]+" | cut -d= -f2)
        MENU=$(grep -oE "mainmenu=[01]" "$RUN_LOG" | tail -1)
        log "M7 autopilot: $K keys over $FRAMES frames, scene changed $S time(s), ${MENU:-mainmenu=?}"
        if [ "$S" -ge "$MIN_SCENES" ]; then
            REACHED=7; log "M7 ok"
        else
            fail "M7: the scene changed $S time(s) (need $MIN_SCENES) - the game draws but does not advance"
            fail "     check which keycode the engine accepts and whether it reads keys at all"
        fi
    fi
fi

echo "$REACHED" > "$RESULTS/milestone"
log "=== milestone reached: $REACHED / 7 ==="
exit $REACHED
