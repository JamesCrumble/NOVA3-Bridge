#!/usr/bin/env bash
#
# Build the rough on-device test package for N.O.V.A. 3.
#
# This is NOT the release packager. It produces build/nova3-test.zip, a
# PortMaster autoinstall zip carrying the loader, the libraries the CFW will not
# have, the launcher and the gptk - and nothing of the game's. The player's own
# files are laid out by hand into ports/nova3/ afterwards:
#
#   ports/nova3/lib/armeabi-v7a/libNOVA3_neon.so
#   ports/nova3/com.gameloft.android.ANMP.GloftN3HM/main.1050.<pkg>.obb
#   ports/nova3/com.gameloft.android.ANMP.GloftN3HM/patch.1070.<pkg>.obb
#
# A released port imports those with eapx instead; that is deliberately absent
# here, and so is the artwork. What is NOT optional is the launcher's
# missing-files screen, because "no game data" has to be a message and not a
# black panel.
#
# Usage (from the port root):
#     ./package_test.sh            build, collect libs, gate glibc, zip
#     ./package_test.sh --no-build use the binary already in build/
set -euo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PORT_DIR"

IMAGE="${BUILD_IMAGE:-nova3-build}"
BIN="build/nova3"
LIBS="build/libs.armhf"
STAGE="build/pkg-test"
ZIP="build/nova3-test.zip"

# emulator.json spells `binary` as "build/nova3" rather than the bare port name
# the template uses, so portbase/tools/port_config.sh derives the wrong
# defaults from it. Every path this script hands those tools is therefore
# explicit - nothing here is left to that default.
run() { docker run --rm -v "$PORT_DIR":/src -w /src "$IMAGE" "$@"; }

if [ "${1:-}" != "--no-build" ]; then
    echo "== build"
    run make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi

[ -f "$BIN" ] || { echo "package_test: no $BIN - run without --no-build" >&2; exit 1; }

VERSION=$(sed -n 's/^#define NOVA3_PORT_VERSION "\(.*\)"$/\1/p' game/port_version.h)
[ -n "$VERSION" ] || { echo "package_test: could not read the version from game/port_version.h" >&2; exit 1; }

echo "== collect libraries the CFW will not have"
run portbase/tools/collect_libs.sh "$BIN" "$LIBS"

# The gate that the qemu harness structurally cannot be: it runs on a modern
# host, and a binary referencing GLIBC_2.34 fails only on the console's older
# linker. ArkOS/AeolusUX ship 2.28-2.31.
echo "== glibc floor"
run portbase/tools/check_glibc_floor.sh "$BIN" "$LIBS"

echo "== stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/nova3/lib/armeabi-v7a"

cp "ports/N.O.V.A. 3.sh" "$STAGE/"
cp "$BIN"                     "$STAGE/nova3/nova3"
cp ports/nova3/nova3.gptk         "$STAGE/nova3/"
cp -R "$LIBS"                 "$STAGE/nova3/libs.armhf"
chmod 755 "$STAGE/N.O.V.A. 3.sh" "$STAGE/nova3/nova3"

# A marker rather than an empty directory: zip preserves empty directories but
# several CFW unzip paths do not, and the point of the file is to be visible on
# the SD card where the player has to put things.
cat > "$STAGE/nova3/PUT_NOVA3_DATA_HERE.txt" <<'EOF'
Put your own copy of N.O.V.A. 3 (build 1.0.7) in this
directory. The port never ships Gameloft's files.

Required layout, relative to this file:

  lib/armeabi-v7a/libNOVA3_neon.so
  com.gameloft.android.ANMP.GloftN3HM/main.1050.com.gameloft.android.ANMP.GloftN3HM.obb
  com.gameloft.android.ANMP.GloftN3HM/patch.1070.com.gameloft.android.ANMP.GloftN3HM.obb

The .so must be the 1.0.7 build; the launcher checks its sha1 before starting
the game.

Nothing here imports an APK yet - extract it on a computer.
EOF

echo "== zip"
rm -f "$ZIP"
( cd "$STAGE" && zip -qr "$PORT_DIR/$ZIP" . )

echo
unzip -l "$ZIP"
echo
echo "package_test: nova3-test.zip v$VERSION ($(du -sk "$ZIP" | cut -f1) KB)"
echo "  Install through PortMaster autoinstall, then copy the game files into"
echo "  ports/nova3/ as PUT_NOVA3_DATA_HERE.txt describes."
