#!/bin/sh
# P0: is there a system ARM32 translator (Tango / native bridge), the original game, the Games app. Read-only.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
A=$P/tools/adb
$A shell '
echo "== abi"; getprop ro.product.cpu.abilist; getprop ro.product.cpu.abilist32; getprop ro.zygote
echo "== native bridge"; getprop ro.dalvik.vm.native.bridge; getprop ro.enable.native.bridge.exec; getprop | grep -iE "tango|native.bridge|houdini" | head
echo "== tango files"; ls -la /system/lib/libtango* /system/lib64/libtango* /system/bin/tango* /system_ext/lib*/libtango* /vendor/lib*/libtango* 2>/dev/null; ls /system/lib 2>/dev/null | head -3; ls -d /system/lib /system/bin/arm 2>&1 | head
echo "== original game"; pm list packages | grep -i gloft; dumpsys package com.gameloft.android.ANMP.GloftN3HM | grep -E "primaryCpuAbi|secondaryCpuAbi|versionName|nativeLibrary|legacyNativeLibraryDir" | head
echo "== games app"; pm list packages | grep -iE "oplus.games|gamespace|oneplus.games|coloros.game"
echo "== game mode"; cmd game list-modes com.eaprules.nova3 2>&1 | head -3; cmd game mode get com.eaprules.nova3 2>&1 | head -2
echo "== android"; getprop ro.build.version.release; getprop ro.build.display.id
'
