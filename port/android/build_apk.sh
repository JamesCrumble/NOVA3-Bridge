#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-$ROOT/../android-sdk}"
BT="$SDK/build-tools/34.0.0"
PLATFORM="$SDK/platforms/android-34/android.jar"
OUT="$ROOT/android/build/apk"
# QEMU_BIN: the static aarch64 qemu-arm to ship. Default: our patched build (../qemu/out, tools/qemu_build.sh),
# else Debian's qemu-user-static.
QEMU="${QEMU_BIN:-$ROOT/../qemu/out/qemu-arm-D.stripped}"
[ -x "$QEMU" ] || QEMU="$ROOT/../downloads/qemu-debian/qemu-arm-static"

if [ ! -x "$BT/aapt2" ] || [ ! -f "$PLATFORM" ]; then
    echo "Android SDK build-tools 34.0.0 and platform 34 are required" >&2
    exit 1
fi
if [ -n "${JAVA_HOME:-}" ]; then
    PATH="$JAVA_HOME/bin:$PATH"
fi
if ! command -v javac >/dev/null; then
    echo "javac not found; install a JDK or set JAVA_HOME" >&2
    exit 1
fi
if [ ! -x "$QEMU" ]; then
    echo "Missing $QEMU" >&2
    exit 1
fi

cd "$ROOT"
rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/gen" "$OUT/assets" "$OUT/res"

if [ ! -f build/nova3 ]; then
    echo "build/nova3 is missing; build the port first" >&2
    exit 1
fi

if [ ! -d android/build/sysroot ]; then
    bash android/make_sysroot.sh
fi

cp android/AndroidManifest.xml "$OUT/AndroidManifest.xml"
cp -a android/res/. "$OUT/res/"
cp build/nova3 "$OUT/assets/nova3"
mkdir -p "$OUT/lib/arm64-v8a"
python3 android/patch_syscalls.py "$QEMU" "$OUT/lib/arm64-v8a/libqemu.so"
# Diagnostic strace for the device; needs Go (cross-compiles to static linux/arm64).
GO="${GO:-$(command -v go || echo /usr/local/go/bin/go)}"
(cd android/tools/mstrace && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 GOFLAGS=-mod=mod \
    "$GO" build -trimpath -ldflags '-s -w' -o "$OUT/lib/arm64-v8a/libmstrace.so" .)
rm -f android/build/sysroot.zip
python3 android/ziptool.py dir android/build/sysroot android/build/sysroot.zip
cp android/build/sysroot.zip "$OUT/assets/sysroot.zip"

"$BT/aapt2" compile --dir "$OUT/res" -o "$OUT/resources.zip"
"$BT/aapt2" link \
    --manifest "$OUT/AndroidManifest.xml" \
    -I "$PLATFORM" \
    --java "$OUT/gen" \
    --min-sdk-version 26 \
    --target-sdk-version 34 \
    -o "$OUT/base.ap_" "$OUT/resources.zip"

find "$OUT/gen" android/src -name '*.java' -print0 | xargs -0 javac \
    -source 8 -target 8 -encoding UTF-8 -Xlint:-options \
    -classpath "$PLATFORM" -d "$OUT/classes"
"$BT/d8" --lib "$PLATFORM" --output "$OUT/dex" \
    $(find "$OUT/classes" -name '*.class')

python3 android/ziptool.py append "$OUT/base.ap_" "$OUT/unsigned.apk" \
    "S:lib/arm64-v8a/libqemu.so=$OUT/lib/arm64-v8a/libqemu.so" \
    "S:lib/arm64-v8a/libmstrace.so=$OUT/lib/arm64-v8a/libmstrace.so" \
    "S:assets/nova3=$OUT/assets/nova3" \
    "S:assets/sysroot.zip=$OUT/assets/sysroot.zip" \
    "D:classes.dex=$OUT/dex/classes.dex"

# Kept outside $OUT so rebuilt APKs keep the same signature and update in place.
KEY="$ROOT/android/build/debug.keystore"
if [ ! -f "$KEY" ]; then
    keytool -genkeypair -keystore "$KEY" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
"$BT/apksigner" sign --ks "$KEY" --ks-pass pass:android \
    --out "$ROOT/NOVA3-ARM32-debug.apk" "$OUT/aligned.apk"
"$BT/apksigner" verify --verbose "$ROOT/NOVA3-ARM32-debug.apk"
echo "APK: $ROOT/NOVA3-ARM32-debug.apk"
