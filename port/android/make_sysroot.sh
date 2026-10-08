#!/usr/bin/env bash
# Assembles a minimal armhf sysroot for running build/nova3 under qemu-arm:
# the NEEDED closure of the loader, SDL, and the Mesa software GL stack.
# Run in WSL from the repo root. Output: android/build/sysroot
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$REPO/android/build/sysroot"
LIBDIRS=("$REPO/build/sdl-arm/lib" /lib/arm-linux-gnueabihf /usr/lib/arm-linux-gnueabihf /usr/arm-linux-gnueabihf/lib)
STRIP=arm-linux-gnueabihf-strip

rm -rf "$OUT"
mkdir -p "$OUT/lib/arm-linux-gnueabihf" "$OUT/usr/lib/arm-linux-gnueabihf/dri" \
         "$OUT/usr/share/glvnd/egl_vendor.d" "$OUT/usr/share/drirc.d" "$OUT/etc"

find_lib() {
    local name="$1" d
    for d in "${LIBDIRS[@]}"; do
        [ -e "$d/$name" ] && { readlink -f "$d/$name"; return 0; }
    done
    return 1
}

needed() { readelf -d "$1" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'; }

declare -A DONE
QUEUE=()

enqueue() {
    local name="$1"
    [ -n "${DONE[$name]:-}" ] && return
    DONE[$name]=1
    QUEUE+=("$name")
}

# dlopen()ed at runtime, so no NEEDED edge leads to them.
for l in libEGL.so.1 libGL.so.1 libGLESv2.so.2 libEGL_mesa.so.0 libgcc_s.so.1 \
         libstdc++.so.6 libnss_files.so.2 libSDL2-2.0.so.0; do
    enqueue "$l"
done

while read -r n; do enqueue "$n"; done < <(needed "$REPO/build/nova3")

DRI=/usr/lib/arm-linux-gnueabihf/dri/swrast_dri.so
cp "$DRI" "$OUT/usr/lib/arm-linux-gnueabihf/dri/swrast_dri.so"
while read -r n; do enqueue "$n"; done < <(needed "$DRI")

i=0
while [ $i -lt ${#QUEUE[@]} ]; do
    name="${QUEUE[$i]}"; i=$((i + 1))
    case "$name" in ld-linux-armhf.so.3) continue ;; esac
    if ! src="$(find_lib "$name")"; then
        echo "MISSING: $name" >&2
        continue
    fi
    cp "$src" "$OUT/lib/arm-linux-gnueabihf/$name"
    while read -r n; do enqueue "$n"; done < <(needed "$src")
done

cp "$(readlink -f /usr/arm-linux-gnueabihf/lib/ld-linux-armhf.so.3)" "$OUT/lib/ld-linux-armhf.so.3"

cp /usr/share/glvnd/egl_vendor.d/50_mesa.json "$OUT/usr/share/glvnd/egl_vendor.d/"
cp /usr/share/drirc.d/00-mesa-defaults.conf "$OUT/usr/share/drirc.d/"
echo 'app:x:10000:10000::/data:/bin/sh' > "$OUT/etc/passwd"
echo 'passwd: files' > "$OUT/etc/nsswitch.conf"

find "$OUT" -type f -name '*.so*' -exec "$STRIP" --strip-unneeded {} + 2>/dev/null || true

echo "files: $(find "$OUT" -type f | wc -l)  size: $(du -sh "$OUT" | cut -f1)"
du -a "$OUT" | sort -rn | head -8
