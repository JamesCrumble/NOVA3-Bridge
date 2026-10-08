#!/usr/bin/env bash
#
# APK triage: decide in one run whether a game is worth porting.
#
# Everything here is read-only inspection of a zip. It answers the three
# questions that decide the project: which ABIs ship, whether the game is a
# NativeActivity, and how much Java has to be faked.
#
# Usage: triage-apk.sh <game.apk>
set -uo pipefail

APK="${1:-}"
[ -f "$APK" ] || { echo "usage: $0 <game.apk>"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Listed once. Piping unzip into `grep -q` makes grep exit at the first match,
# unzip take SIGPIPE, and pipefail declare the whole pipeline failed - which
# silently turned every "is this present?" check into "no".
LISTING="$(unzip -l "$APK")"

say() { printf '\n=== %s ===\n' "$*"; }

say "ABIs"
# Which architectures ship decides whether you need a 32-bit userland at all.
echo "$LISTING" | grep -oE 'lib/[^/]+/' | sort -u | sed 's/^/  /'
if [[ "$LISTING" == *"lib/arm64-v8a/"* ]]; then
    echo "  -> arm64 present: best case, no multiarch needed"
elif [[ "$LISTING" == *"lib/armeabi-v7a/"* ]]; then
    echo "  -> armhf only: the CFW needs a 32-bit userland AND 32-bit GPU libs"
else
    echo "  -> no ARM library found"
fi

say "Engine"
# A known engine means you should not be writing a loader at all.
if [[ "$LISTING" == *"libunity.so"* ]]; then
    echo "  Unity  -> DEAD END, PortMaster has no Unity runtime"
elif [[ "$LISTING" == *"libyoyo.so"* || "$LISTING" == *"game.droid"* ]]; then
    echo "  GameMaker -> use gmloader-next as-is, do not write code"
elif [[ "$LISTING" == *"libgodot"* || "$LISTING" == *"libgdx"* ]]; then
    echo "  Godot/libGDX -> PortMaster has runtimes for these, check first"
else
    echo "  no known engine signature: likely a proprietary engine"
fi

say "Native libraries"
echo "$LISTING" | grep -E '\.so$' | awk '{printf "  %-52s %s KB\n", $4, int($1/1024)}'

say "NativeActivity?"
unzip -o -q "$APK" AndroidManifest.xml -d "$TMP" 2>/dev/null
if [ -f "$TMP/AndroidManifest.xml" ]; then
    python3 - "$TMP/AndroidManifest.xml" <<'PY'
import re, sys
d = open(sys.argv[1], 'rb').read().decode('utf-16-le', errors='ignore')
toks, seen = re.findall(r'[ -~]{4,}', d), []
for t in toks:
    if t not in seen:
        seen.append(t)
literal = any('android.app.NativeActivity' in t for t in seen)
lib_name = any('android.app.lib_name' in t for t in seen)
if literal:
    print("  NativeActivity: YES, declared directly - the game lives in the .so")
elif lib_name:
    print("  NativeActivity: YES, via a subclass - android.app.lib_name is present.")
    print("     The studio subclassed it to add ads/IAP; the engine is still native.")
    print("     That Java class is the one you will have to fake.")
else:
    print("  NativeActivity: no - a Java Activity drives the game, much more work")
for t in seen:
    if 'lib_name' in t:
        i = seen.index(t)
        if i + 1 < len(seen):
            print("  lib_name:", seen[i+1])
    if t.startswith('android.permission') or 'intent.category' in t:
        pass
pkg = [t for t in seen if t.count('.') >= 2 and ' ' not in t and '/' not in t]
if pkg:
    print("  package (best guess):", pkg[0].lstrip('"'))
PY
else
    echo "  could not read the manifest"
fi

say "Java classes the native code calls  <-- this is the workload"
LIB="$(echo "$LISTING" | grep -oE 'lib/(armeabi-v7a|arm64-v8a)/[^ ]*\.so' | head -1)"
if [ -n "$LIB" ]; then
    unzip -o -q "$APK" "$LIB" -d "$TMP"
    N=$(strings -a "$TMP/$LIB" 2>/dev/null | grep -E '^(net/|com/|org/|tv/)' | sort -u | tee "$TMP/classes.txt" | wc -l | tr -d ' ')
    sed 's/^/  /' "$TMP/classes.txt"
    echo "  -> $N class(es) to fake."
    [ "$N" -le 3 ] && echo "     Few: this is a weekend, not a project."
    [ "$N" -gt 10 ] && echo "     Many: budget seriously before starting."
fi

say "Signature (is this the real game?)"
unzip -o -q "$APK" 'META-INF/*.RSA' -d "$TMP" 2>/dev/null
RSA="$(find "$TMP/META-INF" -name '*.RSA' 2>/dev/null | head -1)"
if [ -n "$RSA" ]; then
    openssl pkcs7 -inform DER -in "$RSA" -print_certs -noout 2>/dev/null | head -2 | sed 's/^/  /'
    echo "  (should be the studio. 'Android Debug' means a debug build;"
    echo "   an unrelated name on a game that never shipped on Android is a fake)"
fi

say "Assets"
echo "$LISTING" | grep -oE 'assets/[^ ]*\.[a-zA-Z0-9]+$' | sed 's/.*\.//' | sort | uniq -c | sort -rn | head -8 | sed 's/^/  /'
if [[ "$LISTING" == *".pvr"* ]]; then
    echo "  -> PVRTC textures: those need PowerVR. Mali/Adreno cannot sample them."
else
    echo "  -> no PVRTC found"
fi

say "Verdict"
echo "  Read the table in SKILL.md against the ABI, engine and class count above."
echo "  NativeActivity + few Java classes + portable textures = go."
