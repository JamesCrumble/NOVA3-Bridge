#!/bin/bash
# Builds a release bundle in release/<version>/ (git-ignored):
#   NOVA3-Bridge-<v>.apk          release-signed APK (not debuggable)
#   NOVA3-Bridge-<v>-src.zip      source of the committed HEAD
#   <original game APK>           the player's own copy, to go into Android/data/.../files/
#   *.obb                         game data, to go into Android/obb/com.eaprules.nova3/
#   INSTALL.txt, SHA256SUMS
# Usage: tools/make_release.sh   (run from WSL; commit first - the source zip is HEAD)
set -euo pipefail
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
source "$P/env.sh"
cd "$P"
Q="$P/qemu/out/qemu-arm-${NOVA3_QEMU_VARIANT:-D}.stripped"
VER=$(grep -o 'versionName="[^"]*"' port/android/AndroidManifest.xml | head -1 | cut -d'"' -f2)
OUT="$P/release/$VER"
rm -rf "$OUT"; mkdir -p "$OUT"
bash build.sh > logs/build.log 2>&1
(cd port && QEMU_BIN="$Q" RELEASE=1 bash android/build_apk.sh > "$P/logs/apk_release.log" 2>&1)
tail -2 logs/apk_release.log
mv "port/NOVA3-Bridge-$VER.apk" "$OUT/"
git archive --format=zip --prefix="NOVA3-Bridge-$VER/" -o "$OUT/NOVA3-Bridge-$VER-src.zip" HEAD
G=port/gamebin/N-O-V-A-3-Near-Orbit-Vanguard-Alliance-v1-0-7.apk
ln "$G" "$OUT/" 2>/dev/null || cp "$G" "$OUT/"
for f in nova3data/com.gameloft.android.ANMP.GloftN3HM/*.obb; do ln "$f" "$OUT/" 2>/dev/null || cp "$f" "$OUT/"; done
cp tools/release_install.txt "$OUT/INSTALL.txt"
(cd "$OUT" && sha256sum * > SHA256SUMS)
ls -la "$OUT"
