/*
 * The licence, and the four pointers it locks.
 *
 * GL2JNILib.init() dies on its ninth instruction group with a store through a
 * null pointer:
 *
 *     8d3254  ldr r3, [pc, #2164]     ; GOT offset 0x13dc
 *     8d325c  ldr r3, [fp, r3]        ; fp = PLTGOT -> &lockPointer4
 *     8d3260  ldr r3, [r3]            ; lockPointer4 == NULL
 *     8d3264  str r2, [r3]            ; SIGSEGV at 0x00000000
 *
 * The dump names it without ambiguity: the faulting pc is +0x008d3264, r3 is
 * zero, and the GOT entry at PLTGOT+0x13dc relocates to 0x00db6d8c, which the
 * donor's own symbol table calls `lockPointer4`. Its three siblings sit
 * directly above it at 0xdb6d74, 0xdb6d7c and 0xdb6d88, and the run of GDRM
 * log strings at bea258-bea2c8 (" lock check for set time", " lock attempt",
 * " lock attempt done") is what writes through them.
 *
 * On Android they are allocated by the installer's own native init - the
 * GameInstaller.initNative / GDRMPolicy.initNativeAP entry points, which the
 * Java installer activity calls before the game starts. That activity is a
 * licence check against a Google Play server that has been gone for years, so
 * this port does not run it. What it leaves behind is four null pointers that
 * the engine writes to without checking.
 *
 * Giving them storage is the whole fix, and it is deliberately the *smallest*
 * one: nothing here is patched, no branch is rewritten, no verdict is forged.
 * The engine writes its lock words into four words of ours and reads back what
 * it wrote. The guard that leads to the store is `isCheckLicence == 0`, which
 * is already the state a build with no installer is in.
 */

#include <stdio.h>

#include "so_util.h"
#include "trace.h"

#include "nova3.h"

/*
 * One word per pointer, and they are separate rather than an array because the
 * engine compares the addresses in at least one place ("lockPointer" appears
 * four times in the symbol table, not once with an index) - four words of one
 * array would still be four distinct addresses, but keeping them named makes a
 * later fault in one of them attributable to that one.
 */
static int g_lock_word[4];

void nova3_unlock_drm(so_module *mod)
{
    static const char *const kNames[4] = {
        "lockPointer1", "lockPointer2", "lockPointer3", "lockPointer4",
    };

    for (int i = 0; i < 4; i++) {
        int **slot = (int **)so_symbol(mod, kNames[i]);
        if (!slot) {
            trace("DRM: %s is not exported by this build - if init() faults on "
                  "a null store, this is why", kNames[i]);
            continue;
        }
        g_lock_word[i] = 0;
        *slot = &g_lock_word[i];
    }

    trace("DRM: the four GDRM lock words point at loader storage");
}
