/*
 * A real backtrace for the mapped module, from its .ARM.exidx table.
 *
 * ---------------------------------------------------------------------------
 * Why this exists
 *
 * crash.cpp's other backtrace is a *suspect scan*: it walks the stack word by
 * word and prints everything that points just past a call instruction in the
 * module. That finds every live frame, and it also finds every call site that
 * already returned, with no way to tell them apart. Three separate
 * investigations in this port were sent to the wrong function by exactly that
 * ambiguity - a GetModule id paired with the wrong subsystem slot, a returned
 * EA::SP::IsLogEnabled() frame read as the live one, and a ProtoHttpCreate
 * return address read as a fault site. Each cost an iteration to disbelieve.
 *
 * So this walks the unwind tables instead. The module is not in any
 * dl_iterate_phdr list - our own loader mapped it - so the system unwinder
 * cannot see it, but the table itself is right there: so_util records
 * PT_ARM_EXIDX in so_module as arm_exidx / arm_exidx_size precisely so it can
 * be used after the headers are freed.
 *
 * ---------------------------------------------------------------------------
 * What is implemented, and what deliberately is not
 *
 * ARM EHABI's index is a sorted array of {prel31 function, prel31 entry-or-
 * inline-data} pairs. The second word is one of:
 *
 *   EXIDX_CANTUNWIND (1)   this function cannot be unwound through
 *   bit 31 set             the unwind opcodes are inline, in this word
 *   otherwise              a prel31 pointer to a .ARM.extab entry
 *
 * The opcode interpreter below covers the forms a -fomit-frame-pointer NDK
 * build actually emits: adjust vsp, pop a register mask, set vsp from a
 * register, and finish. The exotic ones - VFP/iWMMX register save ranges, the
 * spare/reserved encodings - are recognised only well enough to be *skipped
 * safely* where their length is known, and to abort the walk where it is not.
 * Aborting is the important part: a half-decoded frame is worse than a short
 * backtrace, because it prints a plausible wrong answer.
 *
 * That is also why this never replaces the suspect scan. It runs first, and
 * crash.cpp still prints the scan afterwards, so a fault this cannot walk
 * degrades to exactly the diagnostic that existed before it.
 *
 * ---------------------------------------------------------------------------
 * Signal-handler rules apply
 *
 * Everything here runs on the faulting thread from inside a signal handler:
 * no allocation, no locks, no stdio. Every load from a guest address goes
 * through a caller-supplied readable() probe, because the whole reason we are
 * here is that some pointer was not what the program thought.
 */
#include "unwind_arm.h"

#include <stdint.h>
#include <string.h>

/* prel31: a 31-bit signed offset, relative to the address of the word itself. */
static uintptr_t prel31_to_addr(const uint32_t *p, uint32_t word)
{
    int32_t offset = (int32_t)(word << 1) >> 1;   /* sign-extend from bit 30 */
    return (uintptr_t)p + (uintptr_t)offset;
}

/*
 * Find the index entry covering pc.
 *
 * The table is sorted by function address, so this is a binary search for the
 * last entry whose function start is <= pc. A linear scan would also work and
 * would be about 16k iterations per frame on this module; inside a signal
 * handler that is not free.
 */
static const uint32_t *exidx_lookup(const ArmUnwindContext *ctx, uintptr_t pc)
{
    const uint32_t *table = (const uint32_t *)ctx->exidx_base;
    size_t          count = ctx->exidx_size / 8;
    if (!table || !count)
        return NULL;

    size_t lo = 0, hi = count - 1;
    const uint32_t *found = NULL;

    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        const uint32_t *entry = &table[mid * 2];

        if (!ctx->readable(entry, 8))
            return NULL;

        uintptr_t fn = prel31_to_addr(entry, entry[0]);
        if (fn <= pc) {
            found = entry;
            if (mid == count - 1)
                break;
            lo = mid + 1;
        } else {
            if (mid == 0)
                break;
            hi = mid - 1;
        }
    }

    return found;
}

/*
 * The opcode stream, as EHABI defines it. Returns false the moment anything is
 * not understood, which stops the walk rather than guessing.
 *
 * vsp lives in regs[13] throughout; "pop" means read from vsp and post-increment.
 */
static bool run_opcodes(const ArmUnwindContext *ctx, const uint8_t *ops,
                        unsigned len, uint32_t *regs)
{
    unsigned i = 0;

    while (i < len) {
        uint8_t op = ops[i++];

        if ((op & 0xc0) == 0x00) {              /* 00xxxxxx: vsp += (n<<2)+4 */
            regs[13] += ((op & 0x3f) << 2) + 4;
        } else if ((op & 0xc0) == 0x40) {       /* 01xxxxxx: vsp -= (n<<2)+4 */
            regs[13] -= ((op & 0x3f) << 2) + 4;
        } else if ((op & 0xf0) == 0x80) {       /* 1000iiii iiiiiiii: pop mask */
            if (i >= len)
                return false;
            uint16_t mask = (uint16_t)(((op & 0x0f) << 8) | ops[i++]);
            if (mask == 0)
                return false;                   /* refuse-to-unwind encoding */

            /* The mask covers r4..r15, low bit first. */
            for (int r = 4; r <= 15; r++) {
                if (!(mask & (1 << (r - 4))))
                    continue;
                const uint32_t *slot = (const uint32_t *)(uintptr_t)regs[13];
                if (!ctx->readable(slot, 4))
                    return false;
                regs[r] = *slot;
                regs[13] += 4;
            }
        } else if ((op & 0xf0) == 0x90) {       /* 1001nnnn: vsp = r[n] */
            uint8_t n = op & 0x0f;
            if (n == 13 || n == 15)
                return false;                   /* reserved as a vsp source */
            regs[13] = regs[n];
        } else if ((op & 0xf8) == 0xa0) {       /* 10100nnn: pop r4..r(4+n) */
            unsigned n = op & 0x07;
            for (unsigned r = 4; r <= 4 + n; r++) {
                const uint32_t *slot = (const uint32_t *)(uintptr_t)regs[13];
                if (!ctx->readable(slot, 4))
                    return false;
                regs[r] = *slot;
                regs[13] += 4;
            }
        } else if ((op & 0xf8) == 0xa8) {       /* 10101nnn: ... plus r14 */
            unsigned n = op & 0x07;
            for (unsigned r = 4; r <= 4 + n; r++) {
                const uint32_t *slot = (const uint32_t *)(uintptr_t)regs[13];
                if (!ctx->readable(slot, 4))
                    return false;
                regs[r] = *slot;
                regs[13] += 4;
            }
            const uint32_t *slot = (const uint32_t *)(uintptr_t)regs[13];
            if (!ctx->readable(slot, 4))
                return false;
            regs[14] = *slot;
            regs[13] += 4;
        } else if (op == 0xb0) {                /* finish */
            break;
        } else if (op == 0xb1) {                /* 10110001 0000iiii: pop r0-r3 */
            if (i >= len)
                return false;
            uint8_t mask = ops[i++];
            if (mask == 0 || (mask & 0xf0))
                return false;
            for (int r = 0; r <= 3; r++) {
                if (!(mask & (1 << r)))
                    continue;
                const uint32_t *slot = (const uint32_t *)(uintptr_t)regs[13];
                if (!ctx->readable(slot, 4))
                    return false;
                regs[r] = *slot;
                regs[13] += 4;
            }
        } else if (op == 0xb2) {                /* vsp += 0x204 + (uleb << 2) */
            uint32_t shift = 0, value = 0, byte;
            do {
                if (i >= len)
                    return false;
                byte = ops[i++];
                value |= (byte & 0x7f) << shift;
                shift += 7;
            } while (byte & 0x80);
            regs[13] += 0x204 + (value << 2);
        } else if (op == 0xb3 || op == 0xc8 || op == 0xc9) {
            /* VFP save ranges: one operand byte, and nothing we need to model -
             * the registers are not part of the frame chain. Skip the byte and
             * the space they occupy is already accounted for by the vsp
             * adjustments around them. */
            if (i >= len)
                return false;
            uint8_t operand = ops[i++];
            regs[13] += (((operand & 0x0f) + 1) * 8);
            if (op == 0xb3)
                regs[13] += 4;                  /* FSTMFDX writes one extra word */
        } else if ((op & 0xf8) == 0xb8 || (op & 0xf8) == 0xd0) {
            /* Short VFP forms with the count in the opcode. */
            unsigned n = op & 0x07;
            regs[13] += (n + 1) * 8;
            if ((op & 0xf8) == 0xb8)
                regs[13] += 4;
        } else {
            /* Anything else is reserved or something this port has never seen.
             * Stop rather than produce a frame nobody can trust. */
            return false;
        }
    }

    return true;
}

bool arm_unwind_step(const ArmUnwindContext *ctx, uint32_t *regs)
{
    uintptr_t pc = regs[15];
    if (!pc)
        return false;

    const uint32_t *entry = exidx_lookup(ctx, pc);
    if (!entry || !ctx->readable(entry, 8))
        return false;

    uint32_t second = entry[1];
    if (second == 1)                    /* EXIDX_CANTUNWIND */
        return false;

    const uint8_t *ops;
    unsigned       len;
    uint8_t        inline_buf[3];

    if (second & 0x80000000u) {
        /*
         * Inline compact model. Only personality 0 fits in the index word; the
         * other two live in .ARM.extab and are handled below.
         */
        if (((second >> 24) & 0x0f) != 0)
            return false;
        inline_buf[0] = (uint8_t)(second >> 16);
        inline_buf[1] = (uint8_t)(second >> 8);
        inline_buf[2] = (uint8_t)(second);
        ops = inline_buf;
        len = 3;
    } else {
        const uint32_t *extab = (const uint32_t *)prel31_to_addr(&entry[1], second);
        if (!ctx->readable(extab, 4))
            return false;

        uint32_t header = extab[0];
        if (!(header & 0x80000000u))
            return false;               /* generic personality: needs a real PR */

        unsigned personality = (header >> 24) & 0x0f;
        if (personality == 0) {
            inline_buf[0] = (uint8_t)(header >> 16);
            inline_buf[1] = (uint8_t)(header >> 8);
            inline_buf[2] = (uint8_t)(header);
            ops = inline_buf;
            len = 3;
        } else if (personality == 1 || personality == 2) {
            unsigned words = (header >> 16) & 0xff;
            if (!words || !ctx->readable(extab, 4 + words * 4))
                return false;
            /* Two opcode bytes in the header word, then `words` more words. */
            static uint8_t buf[4 + 255 * 4];
            buf[0] = (uint8_t)(header >> 8);
            buf[1] = (uint8_t)(header);
            for (unsigned w = 0; w < words; w++) {
                uint32_t v = extab[1 + w];
                buf[2 + w * 4 + 0] = (uint8_t)(v >> 24);
                buf[2 + w * 4 + 1] = (uint8_t)(v >> 16);
                buf[2 + w * 4 + 2] = (uint8_t)(v >> 8);
                buf[2 + w * 4 + 3] = (uint8_t)(v);
            }
            ops = buf;
            len = 2 + words * 4;
        } else {
            return false;
        }
    }

    uint32_t saved_lr = regs[14];

    if (!run_opcodes(ctx, ops, len, regs))
        return false;

    /*
     * If the frame did not restore pc explicitly, the return address is in lr -
     * which is the common case for a leaf-ish frame that only popped r4-r11.
     */
    if (regs[15] == pc)
        regs[15] = regs[14];

    /*
     * Stop on anything that means the chain has ended or gone wrong: a pc that
     * did not move and an lr that did not either, or a null return address.
     * Without this a frame that unwinds to itself loops forever inside a signal
     * handler, which turns a crash into a hang.
     */
    if (!regs[15] || (regs[15] == pc && regs[14] == saved_lr))
        return false;

    return true;
}
