#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ARM EHABI unwinding for a module this loader mapped itself.
 *
 * The system unwinder cannot help: the module is in no dl_iterate_phdr list,
 * so nothing outside this process knows its .ARM.exidx exists. so_util keeps
 * the segment's runtime address in so_module for exactly this.
 *
 * See src/unwind_arm.cpp for what the opcode interpreter covers and, more
 * importantly, what it refuses to guess at.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uintptr_t exidx_base;   /* PT_ARM_EXIDX, where it landed in memory */
    size_t    exidx_size;   /* in bytes; entries are 8 bytes each */

    /*
     * Memory probe. Everything here runs in a signal handler after something
     * already dereferenced a bad pointer, so no load is taken on trust. See
     * crash.cpp's readable(), which asks the kernel via write(2) to /dev/null.
     */
    bool (*readable)(const void *p, size_t len);
} ArmUnwindContext;

/*
 * Advance `regs` (r0-r15, with r13 as vsp and r15 as pc) one frame outward.
 *
 * Returns false when the chain ends, when the table says a function cannot be
 * unwound through, or when anything at all is not understood - a short
 * backtrace is useful and a wrong one is not.
 */
bool arm_unwind_step(const ArmUnwindContext *ctx, uint32_t *regs);

#ifdef __cplusplus
}
#endif
